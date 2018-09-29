from . import py_wrapper

from .batch_helpers import batch_apply, create_batch_client
from .common import flatten, update, get_value, chunk_iter_stream, require, get_disk_size
from .config import get_config
from .errors import YtError
from .format import create_format, YsonFormat, YamrFormat, SkiffFormat
from .ypath import TablePath
from .cypress_commands import exists, get, get_attribute, get_type, remove
from .transaction_commands import abort_transaction
from .file_commands import upload_file_to_cache, is_executable, LocalFile
from .transaction import Transaction, null_transaction_id
from .skiff import convert_to_skiff_schema

import yt.logger as logger
import yt.yson as yson

from yt.packages.six import text_type, binary_type, PY3
from yt.packages.six.moves import map as imap, zip as izip

import time
import types
from copy import deepcopy

try:
    from cStringIO import StringIO as BytesIO
except ImportError:  # Python 3
    from io import BytesIO

import collections
import itertools

DEFAULT_EMPTY_TABLE = TablePath("//sys/empty_yamr_table", simplify=False)

def iter_by_chunks(iterable, count):
    iterator = iter(iterable)
    for first in iterator:
        yield itertools.chain([first], itertools.islice(iterator, count - 1))

def _to_chunk_stream(stream, format, raw, split_rows, chunk_size, rows_chunk_size):
    if isinstance(stream, (text_type, binary_type)):
        if isinstance(stream, text_type):
            if not PY3:
                stream = stream.encode("utf-8")
            else:
                raise YtError("Cannot split unicode string into chunks, consider encoding it first")
        stream = BytesIO(stream)

    iterable_types = [list, types.GeneratorType, collections.Iterator, collections.Iterable]

    is_iterable = isinstance(stream, tuple(iterable_types))
    is_filelike = hasattr(stream, "read")

    if not is_iterable and not is_filelike:
        raise YtError("Cannot split stream into chunks. "
                      "Expected iterable or file-like object, got {0}".format(repr(stream)))

    if raw:
        if is_filelike:
            if split_rows:
                stream = format.load_rows(stream, raw=True)
            else:
                stream = chunk_iter_stream(stream, chunk_size)
        for chunk in stream:
            yield chunk
    else:
        if is_filelike:
            raise YtError("Incorrect input type, it must be generator or list")
        # is_iterable
        if split_rows:
            for row in stream:
                yield format.dumps_row(row)
        else:
            for chunk in iter_by_chunks(stream, rows_chunk_size):
                yield format.dumps_rows(chunk)


def _prepare_command_format(format, raw, client):
    if format is None:
        format = get_config(client)["tabular_data_format"]
    if not raw and format is None:
        format = YsonFormat(boolean_as_string=False)
    if isinstance(format, str):
        format = create_format(format)

    require(format is not None,
            lambda: YtError("You should specify format"))
    return format

def _prepare_source_tables(tables, replace_unexisting_by_empty=True, client=None):
    result = [TablePath(table, client=client) for table in flatten(tables)]
    if not result:
        raise YtError("You must specify non-empty list of source tables")
    if get_config(client)["yamr_mode"]["treat_unexisting_as_empty"]:
        filtered_result = []
        exists_results = batch_apply(exists, result, client=client)

        for table, exists_result in izip(result, exists_results):
            if exists_result:
                filtered_result.append(table)
            else:
                logger.warning("Warning: input table '%s' does not exist", table)
                if replace_unexisting_by_empty:
                    filtered_result.append(DEFAULT_EMPTY_TABLE)
        result = filtered_result
    return result


def _are_default_empty_table(tables):
    return all(table == DEFAULT_EMPTY_TABLE for table in tables)


def _prepare_table_writer(table_writer, client):
    table_writer_from_config = deepcopy(get_config(client)["table_writer"])
    if table_writer is None and not table_writer_from_config:
        return None
    return update(table_writer_from_config, get_value(table_writer, {}))


def _remove_locks(table, client=None):
    for lock_obj in get_attribute(table, "locks", [], client=client):
        if lock_obj["mode"] != "snapshot":
            if exists("//sys/transactions/" + lock_obj["transaction_id"], client=client):
                abort_transaction(lock_obj["transaction_id"], client=client)


def _remove_tables(tables, client=None):
    exists_results = batch_apply(exists, tables, client=client)

    exists_tables = []
    for table, exists_result in izip(tables, exists_results):
        if exists_result:
            exists_tables.append(table)

    type_results = batch_apply(get_type, exists_tables, client=client)

    tables_to_remove = []
    for table, table_type in izip(exists_tables, type_results):
        table = TablePath(table)
        if table_type == "table" and not table.append and table != DEFAULT_EMPTY_TABLE:
            if get_config(client)["yamr_mode"]["abort_transactions_with_remove"]:
                _remove_locks(table, client=client)
            tables_to_remove.append(table)

    batch_apply(remove, tables_to_remove, client=client)

class FileUploader(object):
    def __init__(self, client):
        self.client = client
        self.disk_size = 0
        self.uploaded_files = []

    def __call__(self, files):
        if files is None:
            return []

        file_paths = []
        with Transaction(transaction_id=null_transaction_id, attributes={"title": "Python wrapper: upload operation files"}, client=self.client):
            for file in flatten(files):
                if isinstance(file, (text_type, binary_type, LocalFile)):
                    file_params = {"filename": file}
                else:
                    file_params = deepcopy(file)

                filename = file_params.pop("filename")
                local_file = LocalFile(filename)

                self.disk_size += get_disk_size(local_file.path)

                path = upload_file_to_cache(filename=local_file.path, client=self.client, **file_params)
                file_paths.append(yson.to_yson_type(path, attributes={
                    "executable": is_executable(local_file.path, client=self.client),
                    "file_name": local_file.file_name,
                }))
                self.uploaded_files.append(path)
        return file_paths

def _is_python_function(binary):
    return isinstance(binary, types.FunctionType) or hasattr(binary, "__call__")

def _prepare_format(format, default_format=None):
    if format is None:
        return default_format
    if isinstance(format, str):
        return create_format(format)
    return format

def _prepare_format_from_binary(format, binary, format_type):
    format_from_binary = getattr(binary, "attributes", {}).get(format_type, None)
    if format is None:
        return format_from_binary
    if format_from_binary is None:
        return format
    raise YtError("'{}' specified both implicitely and as function attribute".format(format_type))

def _get_skiff_schema_from_tables(tables, client):
    def _get_schema(table):
        if table is None:
            return None
        try:
            return get(table + "/@schema", client=client)
        except YtError as err:
            if err.is_resolve_error():
                return None
            raise

    schemas = []
    for table in tables:
        schema = _get_schema(table)
        if schema is None:
            return None
        schemas.append(schema)
    return list(imap(convert_to_skiff_schema, schemas))

def _prepare_default_format(binary, format_type, tables, client):
    is_python_function = _is_python_function(binary)
    if is_python_function and getattr(binary, "attributes", {}).get("with_skiff_schemas", False):
        skiff_schema = _get_skiff_schema_from_tables(tables, client)
        if skiff_schema is not None:
            return SkiffFormat(skiff_schema)
    format = _prepare_format(get_config(client)["tabular_data_format"])
    if format is None and is_python_function:
        return YsonFormat(boolean_as_string=False)
    if format is None:
        raise YtError("You should specify " + format_type)
    return format

def _prepare_operation_formats(format, input_format, output_format, binary, input_tables, output_tables, client):
    format = _prepare_format(format)
    input_format = _prepare_format(input_format, format)
    output_format = _prepare_format(output_format, format)

    input_format = _prepare_format_from_binary(input_format, binary, "input_format")
    output_format = _prepare_format_from_binary(output_format, binary, "output_format")

    if input_format is None:
        input_format = _prepare_default_format(binary, "input_format", input_tables, client)
    if output_format is None:
        output_format = _prepare_default_format(binary, "output_format", output_tables, client)

    return input_format, output_format

def _prepare_binary(binary, file_uploader, params, client=None):
    result = None
    if _is_python_function(binary):
        start_time = time.time()
        if isinstance(params.input_format, YamrFormat) and params.group_by is not None \
                and set(params.group_by) != {"key"}:
            raise YtError("Yamr format does not support reduce by %r", params.group_by)
        result = \
            py_wrapper.wrap(function=binary,
                            uploader=file_uploader,
                            params=params,
                            client=client)

        logger.debug("Collecting python modules and uploading to cypress takes %.2lf seconds", time.time() - start_time)
    else:
        result = py_wrapper.WrapResult(cmd=binary, files=[], tmpfs_size=0, environment={}, local_files_to_remove=[], title=None)

    result.environment["YT_ALLOW_HTTP_REQUESTS_TO_YT_FROM_JOB"] = \
       str(int(get_config(client)["allow_http_requests_to_yt_from_job"]))

    return result

def _prepare_destination_tables(tables, client=None):
    from .table_commands import _create_table
    if tables is None:
        if get_config(client)["yamr_mode"]["throw_on_missing_destination"]:
            raise YtError("Destination tables are missing")
        return []
    tables = list(imap(lambda name: TablePath(name, client=client), flatten(tables)))
    batch_client = create_batch_client(raise_errors=True, client=client)
    for table in tables:
        _create_table(table, ignore_existing=True, client=batch_client)
    batch_client.commit_batch()
    return tables

def _prepare_job_io(job_io=None, table_writer=None):
    if job_io is None:
        job_io = {}
    if table_writer is not None:
        job_io.setdefault("table_writer", table_writer)
    return job_io

def _prepare_operation_files(local_files=None, yt_files=None):
    result = []

    if yt_files is not None:
        result += flatten(yt_files)

    local_files = flatten(get_value(local_files, []))
    result += map(LocalFile, local_files)
    return result

def _prepare_stderr_table(name, client=None):
    from .table_commands import _create_table
    if name is None:
        return None

    table = TablePath(name, client=client)
    with Transaction(transaction_id=null_transaction_id, client=client):
        _create_table(table, ignore_existing=True, client=client)
    return table

