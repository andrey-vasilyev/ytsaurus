import fennel

from tornado import iostream
from tornado import ioloop
from tornado import testing
from tornado import gen

import pytest
import mock
import datetime
import unittest
import subprocess
import functools
import logging
import uuid
import sys
import inspect

pytestmark = pytest.mark.skipif("sys.version_info < (2,7)", reason="requires python2.7")


def test_compression_external():
    p = subprocess.Popen(["gzip", "-d"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdoutdata, stderrdata = p.communicate(fennel.gzip_compress("Hello"))
    assert stdoutdata == "Hello"
    assert not stderrdata


def test_compression_internal():
    assert fennel.gzip_decompress(fennel.gzip_compress("Hello")) == "Hello"


def test_event_log_timestamp_parse():
    d = datetime.datetime.strptime("2014-10-02T15:31:24.887386Z".split(".")[0], "%Y-%m-%dT%H:%M:%S")
    assert d.hour == 15
    assert d.minute == 31
    assert d.second == 24
    assert d.date().year == 2014
    assert d.date().month == 10
    assert d.date().day == 2


def test_normilize_timestamp():
    assert fennel.normilize_timestamp("2014-10-02T15:31:24.887386Z") == "2014-10-02 15:31:24"
    assert fennel.normilize_timestamp("2014-10-10T14:07:22.295882Z") == "2014-10-10 14:07:22"


def test_tskv_value_escape_encode():
    assert fennel.escape_encode("Hello") == "Hello"
    assert fennel.escape_encode("Hello\nworld") == "Hello\\nworld"
    assert fennel.escape_encode("\\ is slash, but \0 is zero char") == "\\\\ is slash, but \\0 is zero char"


def test_tskv_key_escape_encode():
    assert fennel.escape_encode("Complex=key", escape_chars=fennel.TSKV_KEY_ESCAPE) == "Complex\\=key"


def check_identity(identity, parameter):
    assert identity(parameter) == parameter


def test_tskv_escape_value_encode_decode():
    def identity(line):
        return fennel.escape_decode(fennel.escape_encode(line))

    check_identity(identity, "\\ is slash, but \0 is zero char")
    check_identity(identity, "Hello")
    check_identity(identity, "Hello\nworld")


def compare_types(left, right):
    if isinstance(left, basestring) and isinstance(right, basestring):
        pass
    else:
        assert type(left) == type(right)


def compare(left, right):
    compare_types(left, right)
    if isinstance(left, dict):
        assert len(left.keys()) == len(right.keys())
        for k in left.keys():
            compare(left[k], right[k])
    else:
        assert left == right


def test_convert_to_from():
    dataset = [
        dict(key1="value1", key2="value2"),
        dict(ks="string", ki=3),
        dict(key=dict(s1="v1", s2="v2"), other_key="data")
#        dict(key="{}")
    ]
    for data in dataset:
        data_after = fennel.convert_from(fennel.convert_to(data))
        compare(data, data_after)


class TestLogBrokerIntegration(testing.AsyncTestCase):
    service_id = "fenneltest"

    def setUp(self):
        super(TestLogBrokerIntegration, self).setUp()
        self.l = None
        self.init()
        self.wait()

    def tearDown(self):
        self.l.stop()
        super(TestLogBrokerIntegration, self).tearDown()

    @gen.coroutine
    def init(self):
        self.source_id = uuid.uuid4().hex

        source_id = uuid.uuid4().hex
        self.l = fennel.LogBroker(service_id=self.service_id, source_id=source_id, io_loop=self.io_loop)
        seqno = yield self.l.connect('kafka01ft.stat.yandex.net')
        assert seqno == 0
        self.stop()

    @testing.gen_test
    def test_save_one_chunk(self):
        seqno = yield self.l.save_chunk(1, [{}])
        assert seqno == 1

    @testing.gen_test
    def test_save_two_concurrent_chunks(self):
        f1 = self.l.save_chunk(1, [{"key": i} for i in xrange(10**3)])
        f2 = self.l.save_chunk(2, [{}])
        seqnos = yield [f1, f2]
        assert  seqnos[0] >= 1
        assert  seqnos[1] == 2

    @testing.gen_test
    def test_save_chunks_unordered(self):
        seqno = yield self.l.save_chunk(20, [{}])
        assert seqno == 20
        seqno = yield self.l.save_chunk(10, [{}])
        assert seqno == 20


class TestSessionStreamIntegraion(testing.AsyncTestCase):
    @testing.gen_test
    def test_connect(self):
        s = fennel.SessionStream(io_loop=self.io_loop)
        session_id = yield s.connect((u'kafka01ft.stat.yandex.net', 80))
        assert session_id is not None
        assert "seqno" in s.get_attributes()

    @testing.gen_test
    def test_get_ping(self):
        s = fennel.SessionStream(io_loop=self.io_loop)
        session_id = yield s.connect((u'kafka01ft.stat.yandex.net', 80))
        assert session_id is not None
        message = yield s.read_message()
        assert message.type == "ping"


class TestSessionPushStreamIntegration(testing.AsyncTestCase):
    hostname = "kafka01ft.stat.yandex.net"
    service_id = "fenneltest"

    def setUp(self):
        super(TestSessionPushStreamIntegration, self).setUp()
        self.init()
        self.wait()

    @gen.coroutine
    def init(self):
        self.source_id = uuid.uuid4().hex

        self.s = fennel.SessionStream(service_id=self.service_id, source_id=self.source_id, io_loop=self.io_loop)
        session_id = yield self.s.connect((self.hostname, 80))
        assert session_id is not None

        self.p = fennel.PushStream(io_loop=self.io_loop)
        yield self.p.connect((self.hostname, 9000), session_id=session_id)
        self.stop()

    def write_chunk(self, seqno):
        data = [{}]
        serialized_data = fennel.serialize_chunk(0, seqno, 0, data)
        return self.p.write_chunk(serialized_data)

    @testing.gen_test
    def test_basic(self):
        yield self.write_chunk(1)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "ack"
        assert message.attributes["seqno"] == 1

    @testing.gen_test
    def test_two_consecutive(self):
        yield self.write_chunk(1)
        yield self.write_chunk(2)

        acked_seqno = 0
        while acked_seqno < 2:
            message = None
            while message is None or message.type == "ping":
                message = yield self.s.read_message()
            assert message.type == "ack"
            acked_seqno = message.attributes["seqno"]
        assert acked_seqno == 2

    @testing.gen_test
    def test_two_non_consecutive(self):
        yield self.write_chunk(10)
        yield self.write_chunk(20)

        acked_seqno = 0
        while acked_seqno < 20:
            message = None
            while message is None or message.type == "ping":
                message = yield self.s.read_message()
            assert message.type == "ack"
            acked_seqno = message.attributes["seqno"]
        assert acked_seqno == 20

    @testing.gen_test
    def test_two_unordered(self):
        yield self.write_chunk(20)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "ack"
        assert message.attributes["seqno"] == 20

        yield self.write_chunk(10)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "skip"
        assert message.attributes["seqno"] == 10

    @testing.gen_test
    def test_two_equal(self):
        yield self.write_chunk(20)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "ack"
        assert message.attributes["seqno"] == 20

        yield self.write_chunk(20)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "skip"
        assert message.attributes["seqno"] == 20

    @testing.gen_test
    def test_session_seqno(self):
        yield self.write_chunk(20)

        message = None
        while message is None or message.type == "ping":
            message = yield self.s.read_message()
        assert message.type == "ack"
        assert message.attributes["seqno"] == 20

        self.p.stop()
        self.s.stop()

        self.s = fennel.SessionStream(service_id=self.service_id, source_id=self.source_id, io_loop=self.io_loop)
        session_id = yield self.s.connect((self.hostname, 80))
        assert session_id is not None

        self.p = fennel.PushStream(io_loop=self.io_loop)
        yield self.p.connect((self.hostname, 9000), session_id=session_id)

        assert int(self.s.get_attributes()["seqno"]) == 20


# Good tests

class FakeIOStream(object):
    log = logging.getLogger("FakeIOStream")

    IGNORE = object()

    def __init__(self, description=None):
        self._description = description or []
        self._index = 0

    def _compare_lists(self, expected, actual):
        assert len(expected) == len(actual)
        for i in xrange(len(expected)):
            if expected[i] != FakeIOStream.IGNORE:
                assert expected[i] == actual[i]

    def _compare_dicts(self, expected, actual):
        assert len(expected) == len(actual)
        for key, value in expected.iteritems():
            if value != FakeIOStream.IGNORE:
                assert key in actual
                assert value == actual[key]

    def method_missing(self, name, *args, **kwargs):
        self.log.info("%s called with %r and %r", name, args, kwargs)

        expected_name, expected_args, expected_kwargs, return_value = self._description[self._index]
        assert expected_name == name
        self._compare_lists(expected_args, args)
        self._compare_dicts(expected_kwargs, kwargs)
        self._index += 1
        return return_value

    def expect(self, *calls):
        self._description.extend(calls)

    def __getattr__(self, name):
        return functools.partial(self.method_missing, name)


def call(name, return_value, *args, **kwargs):
    rv = gen.Future()
    if issubclass(type(return_value), Exception):
        rv.set_exception(return_value)
    else:
        rv.set_result(return_value)
    return (name, args, kwargs, rv)


def test_fake_io_stream():
    f = FakeIOStream([call("hello", "hi")])
    assert f.hello().result() == "hi"


class TestSessionStream(testing.AsyncTestCase):
    good_response = """HTTP/1.1 200 OK
Server: nginx/1.4.4
Date: Wed, 19 Mar 2014 11:09:54 GMT
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: keep-alive
Vary: Accept-Encoding
Session: 00291e7c-eedf-42cd-99cc-f18331b9db77
Seqno: 3488
Lines: 218
PartOffset: 396
Topic: rt3.fol--other
Partition: 0\r\n\r\n"""

    session_id_missing_response = """HTTP/1.1 200 OK
Server: nginx/1.4.4
Date: Wed, 19 Mar 2014 11:09:54 GMT
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: keep-alive
Vary: Accept-Encoding\r\n\r\n"""

    endpoint = (u'kafka01ft.stat.yandex.net', 80)

    def setUp(self):
        super(TestSessionStream, self).setUp()
        self.fake_stream = FakeIOStream()
        self.s = fennel.SessionStream(connection_factory=self)
        self._connect_failures = 0

    def connect(self, hostname, port):
        future = gen.Future()
        if self._connect_failures > 0:
            self._connect_failures -= 1
            future.set_exception(IOError())
        else:
            future.set_result(self.fake_stream)
        return future

    @testing.gen_test
    def test_basic(self):
        self.fake_stream.expect(
            call("write", None, FakeIOStream.IGNORE),
            call("read_until", self.good_response, "\r\n\r\n", max_bytes=FakeIOStream.IGNORE),
        )
        result = yield self.s.connect(self.endpoint)
        assert result == "00291e7c-eedf-42cd-99cc-f18331b9db77"

    @testing.gen_test
    def test_reconnect(self):
        self._connect_failures = 1
        self.fake_stream.expect(
            call("write", None, FakeIOStream.IGNORE),
            call("read_until", self.good_response, "\r\n\r\n", max_bytes=FakeIOStream.IGNORE),
        )
        result = yield self.s.connect(self.endpoint)
        assert result == "00291e7c-eedf-42cd-99cc-f18331b9db77"

    @testing.gen_test
    def test_no_session_id(self):
        self.fake_stream.expect(
            call("write", None, FakeIOStream.IGNORE),
            call("read_until", self.session_id_missing_response, "\r\n\r\n", max_bytes=FakeIOStream.IGNORE),
            call("write", None, FakeIOStream.IGNORE),
            call("read_until", self.good_response, "\r\n\r\n", max_bytes=FakeIOStream.IGNORE),
        )
        result = yield self.s.connect(self.endpoint)
        assert result == "00291e7c-eedf-42cd-99cc-f18331b9db77"

    @testing.gen_test
    def test_read_message(self):
        self.fake_stream.expect(
            call("write", None, FakeIOStream.IGNORE),
            call("read_until", self.good_response, "\r\n\r\n", max_bytes=FakeIOStream.IGNORE),
            call("read_until", "4\r\n", "\r\n", max_bytes=FakeIOStream.IGNORE),
            call("read_bytes", "ping\r\n", 6),
        )
        yield self.s.connect(self.endpoint)
        message = yield self.s.read_message()
        assert message.type == "ping"
