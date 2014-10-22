#!/usr/bin/env python

import yt.logger as logger
from yt.tools.yamr import Yamr
from yt.tools.remote_copy_tools import \
    copy_yamr_to_yt_pull, \
    copy_yt_to_yamr_pull, \
    copy_yt_to_yamr_push, \
    copy_yt_to_kiwi, \
    copy_yt_to_yt_through_proxy, \
    run_operation_and_notify, \
    Kiwi
from yt.wrapper.client import Yt
from yt.wrapper.common import generate_uuid, get_value
import yt.wrapper as yt

from flask import Flask, request, jsonify, Response, make_response

import os
import json
import time
import signal
import socket
import logging
import argparse
import traceback
from copy import deepcopy
from datetime import datetime
from collections import defaultdict

from threading import RLock, Thread
from multiprocessing import Process, Queue

class RequestFailed(yt.YtError):
    pass

class IncorrectTokenError(RequestFailed):
    pass

def now():
    return str(datetime.utcnow().isoformat() + "Z")

def create_pool(yt_client, destination_cluster_name):
    pool_name = "transfer_" + destination_cluster_name
    pool_path = "//sys/pools/transfer_manager/" + pool_name
    if not yt_client.exists(pool_path):
        yt_client.create("map_node", pool_path, recursive=True, ignore_existing=True)
    yt_client.set(pool_path + "/@resource_limits", {"user_slots": 200})
    yt_client.set(pool_path + "/@mode", "fifo")
    return pool_path

def get_pool(yt_client, name):
    if name in yt_client._pools:
        return yt_client._pools[name]
    else:
        return create_pool(yt_client, name)

class Task(object):
    # NB: destination table is missing if we copy to kiwi
    def __init__(self, source_cluster, source_table, destination_cluster, creation_time, id, state, user,
                 destination_table=None, source_cluster_token=None, token=None, destination_cluster_token=None, mr_user=None, error=None,
                 start_time=None, finish_time=None, copy_method=None, progress=None, meta=None):
        self.source_cluster = source_cluster
        self.source_table = source_table
        self.source_cluster_token = get_value(source_cluster_token, token)
        self.destination_cluster = destination_cluster
        self.destination_table = destination_table
        self.destination_cluster_token = get_value(destination_cluster_token, token)

        self.creation_time = creation_time
        self.start_time = start_time
        self.finish_time = finish_time
        self.state = state
        self.id = id
        self.user = user
        self.mr_user = mr_user
        self.error = error
        self.token = get_value(token, "")
        self.copy_method = get_value(copy_method, "pull")
        self.progress = progress

        # Special field to store meta-information for web-interface
        self.meta = meta

    def get_queue_id(self):
        return self.source_cluster, self.destination_cluster

    def get_source_client(self, clusters):
        if self.source_cluster not in clusters:
            raise yt.YtError("Unknown cluster " + self.source_cluster)
        client = deepcopy(clusters[self.source_cluster])
        if client._type == "yt":
            client.token = self.source_cluster_token
        if client._type == "yamr" and self.mr_user is not None:
            client.mr_user = self.mr_user
        return client

    def get_destination_client(self, clusters):
        if self.destination_cluster not in clusters:
            raise yt.YtError("Unknown cluster " + self.destination_cluster)
        client = deepcopy(clusters[self.destination_cluster])
        if client._type == "yt":
            client.token = self.destination_cluster_token
        if client._type == "yamr" and self.mr_user is not None:
            client.mr_user = self.mr_user
        return client

    def dict(self, hide_token=False):
        result = deepcopy(self.__dict__)
        if hide_token:
            for key in ["token", "source_cluster_token", "destination_cluster_token"]:
                del result[key]
        for key in result.keys():
            if result[key] is None:
                del result[key]
        return result

class Application(object):
    ERROR_BUFFER_SIZE = 2 ** 16

    def __init__(self, config):
        self._daemon = Flask(__name__)

        self._config = config
        self._mutex = RLock()
        self._yt = Yt(config["proxy"])
        self._yt.token = config["token"]

        self._load_config(config)
        self._path = config["path"]

        self._sleep_step = 0.5

        # Prepare auth node if it is missing
        self._auth_path = os.path.join(config["path"], "auth")
        self._yt.create("map_node", self._auth_path, ignore_existing=True)

        # Run lock thread
        self._terminating = False
        self._lock_acquired = False
        self._lock_path = os.path.join(config["path"], "lock")
        self._lock_timeout = 10.0
        self._yt.create("map_node", self._lock_path, ignore_existing=True)
        self._lock_thread = Thread(target=self._take_lock)
        self._lock_thread.daemon = True
        self._lock_thread.start()

        # Run execution thread
        self._task_processes = {}
        self._execution_thread = Thread(target=self._execute_tasks)
        self._execution_thread.daemon = True
        self._execution_thread.start()

        # Add rules
        self._add_rule("/", 'main', methods=["GET"], depends_on_lock=False)
        self._add_rule("/tasks/", 'get_tasks', methods=["GET"])
        self._add_rule("/tasks/", 'add', methods=["POST"])
        self._add_rule("/tasks/<id>/", 'get_task', methods=["GET"])
        self._add_rule("/tasks/<id>/", 'delete_task', methods=["DELETE"])
        self._add_rule("/tasks/<id>/abort/", 'abort', methods=["POST"])
        self._add_rule("/tasks/<id>/restart/", 'restart', methods=["POST"])
        self._add_rule("/config/", 'config', methods=["GET"])
        self._add_rule("/ping/", 'ping', methods=["GET"])

    def terminate(self):
        self._terminating = True
        self._lock_thread.join(self._sleep_step)
        self._execution_thread.join(self._sleep_step)

    def _add_rule(self, rule, endpoint, methods, depends_on_lock=True):
        methods.append("OPTIONS")
        self._daemon.add_url_rule(
            rule,
            endpoint,
            self._process_lock(
                self._process_cors(
                    self._process_exception(
                        Application.__dict__[endpoint]
                    ),
                    methods),
                depends_on_lock),
            methods=methods)

    def _process_lock(self, func, depends_on_lock):
        def decorator(*args, **kwargs):
            if depends_on_lock and not self._lock_acquired:
                return "Cannot take lock", 500
            return func(*args, **kwargs)
        return decorator

    def _process_cors(self, func, methods):
        def decorator(*args, **kwargs):
            if request.method == "OPTIONS":
                rsp = self._daemon.make_default_options_response()
                rsp.headers["Access-Control-Allow-Origin"] = "*"
                rsp.headers["Access-Control-Allow-Methods"] = ", ".join(methods)
                rsp.headers["Access-Control-Allow-Headers"] = ", ".join(["Authorization", "Origin", "Content-Type", "Accept"])
                rsp.headers["Access-Control-Max-Age"] = 3600
                return rsp
            else:
                rsp = make_response(func(self, *args, **kwargs))
                rsp.headers["Access-Control-Allow-Origin"] = "*"
                return rsp

        return decorator

    def _process_exception(self, func):
        def decorator(*args, **kwargs):
            try:
                return func(*args, **kwargs)
            except RequestFailed as error:
                logger.exception(yt.errors.format_error(error))
                return json.dumps(error.simplify()), 400
            except Exception as error:
                logger.exception(error)
                return json.dumps({"code": 1, "message": "Unknown error: " + str(error)}), 502

        return decorator

    def _take_lock(self):
        yt_client = deepcopy(self._yt)
        try:
            with yt_client.PingableTransaction():
                while True:
                    try:
                        yt_client.lock(self._lock_path)
                        break
                    except yt.YtError as err:
                        if err.is_concurrent_transaction_lock_conflict():
                            logger.info("Failed to take lock")
                            time.sleep(self._lock_timeout)
                            continue
                        logger.exception(yt.errors.format_error(err))
                        return

                logger.info("Lock acquired")

                # Loading tasks from cypress
                self._load_tasks(os.path.join(self._path, "tasks"))

                self._lock_acquired = True


                # Set attribute outside of transaction
                self._yt.set_attribute(self._path, "address", socket.getfqdn())

                # Sleep infinitely long
                while True:
                    if self._terminating:
                        return
                    time.sleep(self._sleep_step)

        except KeyboardInterrupt:
            # Do not print backtrace in case of SIGINT
            pass

    def _load_config(self, config):
        self._configure_logging(config.get("logging", {}))

        self._clusters = {}
        for name, cluster_description in config["clusters"].iteritems():
            type = cluster_description["type"]
            options = cluster_description["options"]

            if type == "yt":
                self._clusters[name] = Yt(**options)
                self._clusters[name]._pools = cluster_description.get("pools", {})
                self._clusters[name]._network = cluster_description.get("remote_copy_network")
                self._clusters[name]._version = cluster_description.get("version", 0)
            elif type == "yamr":
                if "viewer" in options:
                    del options["viewer"]
                self._clusters[name] = Yamr(**options)
            elif type == "kiwi":
                self._clusters[name] = Kiwi(**options)
            else:
                raise yt.YtError("Incorrect cluster type " + options["type"])

            self._clusters[name]._name = name
            self._clusters[name]._type = type

        for name in config["availability_graph"]:
            if name not in self._clusters:
                raise yt.YtError("Incorrect availability graph, cluster {} is missing".format(name))
            for neighbour in config["availability_graph"][name]:
                if neighbour not in self._clusters:
                    raise yt.YtError("Incorrect availability graph, cluster {} is missing".format(neighbour))

        self.kiwi_transmitter = None
        if "kiwi_transmitter" in config:
            self.kiwi_transmitter = Yt(**config["kiwi_transmitter"])

        self._availability_graph = config["availability_graph"]

    def _configure_logging(self, logging_node):
        level = logging.__dict__[logging_node.get("level", "INFO")]

        if "filename" in logging_node:
            handler = logging.FileHandler(logging_node["filename"])
        else:
            handler = logging.StreamHandler()

        new_logger = logging.getLogger("Transfer manager")
        new_logger.propagate = False
        new_logger.setLevel(level)
        new_logger.addHandler(handler)
        new_logger.handlers[0].setFormatter(logger.BASIC_FORMATTER)
        logger.LOGGER = new_logger

        logging.getLogger('werkzeug').setLevel(level)
        logging.getLogger('werkzeug').addHandler(handler)

    def _load_tasks(self, tasks_path): #, archived_tasks_path):
        logger.info("Loading tasks from cypress")

        self._tasks_path = tasks_path
        if not self._yt.exists(self._tasks_path):
            self._yt.create("map_node", self._tasks_path)
        #self._archived_tasks_path = archived_tasks_path

        # From id to task description
        self._tasks = {}

        # From ... to task ids
        self._running_task_queues = defaultdict(lambda: [])

        # List of tasks sorted by creation time
        self._pending_tasks = []

        for id, options in self._yt.get(tasks_path).iteritems():
            task = Task(**options)
            self._tasks[id] = task
            if task.state == "running":
                self._change_task_state(id, "pending")
                task.state = "pending"
            if task.state == "pending":
                self._pending_tasks.append(task.id)

        self._pending_tasks.sort(key=lambda id: self._tasks[id].creation_time)

        logger.info("Tasks load")

    def _change_task_state(self, id, new_state):
        with self._mutex:
            self._tasks[id].state = new_state
            self._yt.set(os.path.join(self._tasks_path, id), self._tasks[id].dict())

    def _dump_task(self, id):
        with self._mutex:
            self._yt.set(os.path.join(self._tasks_path, id), self._tasks[id].dict())

    def _get_token(self, authorization_header):
        words = authorization_header.split()
        if len(words) != 2 or words[0].lower() != "oauth":
            return None
        return words[1]

    def _get_token_and_user(self, authorization_header):
        token = self._get_token(request.headers.get("Authorization", ""))
        if token is None or token == "undefined":
            user = "guest"
            token = ""
        else:
            user = self._yt.get_user_name(token)
            if not user:
                raise IncorrectTokenError("Authorization token is incorrect: " + token)
        return token, user

    def _check_permission(self, id, authorization_header):
        _, user = self._get_token_and_user(authorization_header)
        if self._tasks[id].user != user and \
           self._yt.check_permission(user, "administer", self._auth_path)["action"] != "allow":
            raise RequestFailed("There is no permission to abort task.")

    def _precheck(self, task):
        logger.info("Making precheck for task %s", task.id)
        source_client = task.get_source_client(self._clusters)
        destination_client = task.get_destination_client(self._clusters)

        if task.copy_method not in ["pull", "push"]:
            raise yt.YtError("Incorrect copy method: " + str(task.copy_method))

        if task.source_cluster not in self._availability_graph or \
           task.destination_cluster not in self._availability_graph[task.source_cluster]:
            raise yt.YtError("Cluster {} not available from {}".format(task.destination_cluster, task.source_cluster))

        if source_client._type == "yamr" and source_client.is_empty(task.source_table) or \
           source_client._type == "yt" and (
                not source_client.exists(task.source_table) or
                source_client.get_attribute(task.source_table, "row_count") == 0):
            raise yt.YtError("Source table {} is empty".format(task.source_table))

        if source_client._type == "yt" and destination_client._type == "yamr":
            path = yt.TablePath(task.source_table, end_index=1, simplify=False)
            keys = list(source_client.read_table(path, format=yt.JsonFormat(), raw=False).next())
            if set(keys + ["subkey"]) != set(["key", "subkey", "value"]):
                raise yt.YtError("Keys in the source table must be a subset of ('key', 'subkey', 'value')")

        if destination_client._type == "yt":
            destination_dir = os.path.dirname(task.destination_table)
            if not destination_client.exists(destination_dir):
                raise yt.YtError("Destination directory {} should exist".format(destination_dir))
            destination_user = self._yt.get_user_name(task.destination_cluster_token)
            if destination_user is None or destination_client.check_permission(destination_user, "write", destination_dir)["action"] != "allow":
                raise yt.YtError("There is no permission to write to {}. Please log in.".format(task.destination_table))
        
        if destination_client._type == "kiwi" and self.kiwi_transmitter is None:
            raise yt.YtError("Transimission cluster for transfer to kiwi is not configured")

        logger.info("Precheck for task %s completed", task.id)


    def _can_run(self, task):
        return not self._running_task_queues[task.get_queue_id()]

    def _execute_tasks(self):
        while True:
            if self._terminating:
                return

            logger.info("Processing tasks")
            if not self._lock_acquired:
                time.sleep(self._lock_timeout)
                continue

            with self._mutex:
                self._pending_tasks = filter(lambda id: self._tasks[id].state == "pending", self._pending_tasks)

                logger.info("Progress: %d running, %d pending tasks found", len(self._task_processes), len(self._pending_tasks))

                for id, (process, message_queue) in self._task_processes.items():
                    error = None
                    while not message_queue.empty():
                        message = None
                        try:
                            message = message_queue.get()
                        except Queue.Empty:
                            break
                        if message["type"] == "error":
                            assert not process.is_alive()
                            error = message["error"]
                        elif message["type"] == "operation_started":
                            self._tasks[id].progress["operations"].append(message["operation"])
                            self._dump_task(id)
                        else:
                            assert False, "Incorrect message type: " + message["type"]

                    if not process.is_alive():
                        self._tasks[id].finish_time = now()
                        if process.aborted:
                            pass
                        elif error is None:
                            self._change_task_state(id, "completed")
                        else:
                            self._tasks[id].error = error
                            self._change_task_state(id, "failed")

                        self._dump_task(id)
                        self._running_task_queues[self._tasks[id].get_queue_id()].remove(id)
                        del self._task_processes[id]

                for id in self._pending_tasks:
                    if not self._can_run(self._tasks[id]):
                        logger.info("Task %s cannot be run", id)
                        continue
                    self._running_task_queues[self._tasks[id].get_queue_id()].append(id)
                    self._tasks[id].start_time = now()
                    self._tasks[id].progress = {"operations": []}
                    self._change_task_state(id, "running")
                    queue = Queue()
                    task_process = Process(target=lambda: self._execute_task(self._tasks[id], queue))
                    task_process.aborted = False
                    task_process.start()
                    self._task_processes[id] = (task_process, queue)

            time.sleep(self._sleep_step)

    def _execute_task(self, task, message_queue):
        logger.info("Executing task %s", task.id)
        try:
            self._precheck(task)

            title = "Supervised by transfer task " + task.id
            task_spec = {"title": title, "transfer_task_id": task.id}

            source_client = task.get_source_client(self._clusters)
            destination_client = task.get_destination_client(self._clusters)

            if source_client._type == "yt" and destination_client._type == "yt":
                logger.info("Running YT -> YT remote copy operation")
                if source_client._version != destination_client._version:
                    task_spec["pool"] = get_pool(destination_client, source_client._name)
                    copy_yt_to_yt_through_proxy(
                        source_client,
                        destination_client,
                        task.source_table,
                        task.destination_table,
                        spec_template=task_spec)
                else:
                    run_operation_and_notify(
                        message_queue,
                        destination_client,
                        lambda client, strategy:
                            client.run_remote_copy(
                                task.source_table,
                                task.destination_table,
                                cluster_name=source_client._name,
                                network_name=source_client._network,
                                spec=task_spec,
                                remote_cluster_token=task.source_cluster_token,
                                strategy=strategy))
            elif source_client._type == "yt" and destination_client._type == "yamr":
                logger.info("Running YT -> YAMR remote copy")
                if task.copy_method == "push":
                    task_spec["pool"] = get_pool(source_client, destination_client._name)
                    copy_yt_to_yamr_push(
                        source_client,
                        destination_client,
                        task.source_table,
                        task.destination_table,
                        spec_template=task_spec,
                        message_queue=message_queue)
                else:
                    copy_yt_to_yamr_pull(
                        source_client,
                        destination_client,
                        task.source_table,
                        task.destination_table,
                        message_queue=message_queue)
            elif source_client._type == "yamr" and destination_client._type == "yt":
                task_spec["pool"] = get_pool(destination_client, source_client._name)
                logger.info("Running YAMR -> YT remote copy")
                copy_yamr_to_yt_pull(
                    source_client,
                    destination_client,
                    task.source_table,
                    task.destination_table,
                    spec_template=task_spec,
                    message_queue=message_queue)
            elif source_client._type == "yamr" and destination_client._type == "yamr":
                destination_client.remote_copy(
                    source_client.server,
                    task.source_table,
                    task.destination_table)
            elif source_client._type == "yt" and destination_client._type == "kiwi":
                copy_yt_to_kiwi(source_client, destination_client, self.kiwi_transmitter, task.source_table)
            else:
                raise Exception("Incorrect cluster types: {} source and {} destination".format(
                                source_client._type,
                                destination_client._type))
            logger.info("Task %s completed", task.id)
        except KeyboardInterrupt:
            pass
        except yt.YtError as error:
            logger.exception("Task {} failed with error {}".format(task.id, yt.errors.format_error(error)))
            message_queue.put({
                "type": "error",
                "error": error.simplify()
            })
        except Exception as error:
            logger.exception("Task {} failed with error:".format(task.id))
            message_queue.put({
                "type": "error",
                "error": {
                    "message": "\n".join([traceback.format_exc(), str(error)]),
                    "code": 1
                }
            })

    def _get_task_description(self, task):
        task_description = task.dict(hide_token=True)
        queue_index = 1
        with self._mutex:
            for id in self._pending_tasks:
                if id == task.id:
                    task_description["queue_index"] = queue_index
                if self._tasks[id].get_queue_id() == task.get_queue_id():
                    queue_index += 1
        return task_description

    # Public interface
    def run(self, *args, **kwargs):
        self._daemon.run(*args, **kwargs)


    # Url handlers
    def main(self):
        return "This is YT transfer manager"

    def add(self):
        try:
            params = json.loads(request.data)
        except ValueError as error:
            raise RequestFailed("Cannot parse json from body '{}'".format(request.data), inner_errors=[yt.YtError(error.message)])

        id = generate_uuid()
        logger.info("Adding task %s with id %s", json.dumps(params), id)

        # Move this check to precheck function
        required_parameters = set(["source_cluster", "source_table", "destination_cluster"])
        if not set(params) >= required_parameters:
            raise RequestFailed("All required parameters ({}) must be presented".format(", ".join(required_parameters)))
        if "destination_table" not in params:
            params["destination_table"] = None

        token, user = self._get_token_and_user(request.headers.get("Authorization", ""))

        if "mr_user" not in params:
            params["mr_user"] = self._config.get("default_mr_user")

        try:
            task = Task(id=id, creation_time=now(), user=user, token=token, state="pending", **params)
        except TypeError as error:
            raise RequestFailed("Cannot create task", inner_errors=[yt.YtError(error.message)])

        try:
            self._precheck(task)
        except yt.YtError as error:
            raise RequestFailed("Precheck failed", inner_errors=[error])

        if not request.args.get("dry_run", False):
            with self._mutex:
                self._tasks[task.id] = task
                self._pending_tasks.append(task.id)

            self._yt.set(os.path.join(self._tasks_path, task.id), task.dict())

        logger.info("Task %s added", task.id)

        return task.id

    def abort(self, id):
        if id not in self._tasks:
            raise RequestFailed("Unknown task " + id)

        logger.info("Aboring task %s", id)

        self._check_permission(id, request.headers.get("Authorization", ""))

        if id in self._task_processes:
            process, _ = self._task_processes[id]
            process.aborted = True

            os.kill(process.pid, signal.SIGINT)
            time.sleep(0.5)
            if process.is_alive():
                process.terminate()

        if self._tasks[id].state not in ["aborted", "completed", "failed"]:
            with self._mutex:
                self._change_task_state(id, "aborted")

        return ""

    def restart(self, id):
        if id not in self._tasks:
            raise RequestFailed("Unknown task " + id)

        logger.info("Restarting task %s", id)

        self._check_permission(id, request.headers.get("Authorization", ""))
        if self._tasks[id].state not in ["completed", "aborted", "failed"]:
            raise RequestFailed("Cannot restart task in state " + self._tasks[id].state)

        self._tasks[id].state = "pending"
        self._tasks[id].creation_time = now()
        self._tasks[id].finish_time = None
        self._tasks[id].progress = None
        self._tasks[id].error = None
        self._dump_task(id)
        self._pending_tasks.append(id)

        return ""

    def get_task(self, id):
        if id not in self._tasks:
            return "Unknown task " + id, 400

        return jsonify(**self._get_task_description(self._tasks[id]))

    def delete_task(self, id):
        if id not in self._tasks:
            return "Unknown task " + id, 400

        self._check_permission(id, request.headers.get("Authorization", ""))
        if self._tasks[id].state not in ["completed", "aborted", "failed"]:
            raise RequestFailed("Cannot delete running task " + self._tasks[id].state)

        with self._mutex:
            self._yt.remove(os.path.join(self._tasks_path, id), recursive=True)
        del self._tasks[id]

        return ""

    def get_tasks(self):
        user = request.args.get("user")
        tasks = self._tasks.values()
        if user is not None:
            tasks = [task.user == user for task in tasks]

        return Response(json.dumps(map(self._get_task_description, tasks)), mimetype='application/json')

    def config(self):
        return jsonify(self._config)

    def ping(self):
        return "OK", 200

DEFAULT_CONFIG = {
    "clusters": {
        "kant": {
            "type": "yt",
            "options": {
                "proxy": "kant.yt.yandex.net",
                "hosts": "hosts/fb"
            },
            "remote_copy_network": "fastbone",
        },
        "smith": {
            "type": "yt",
            "options": {
                "proxy": "smith.yt.yandex.net",
                "hosts": "hosts/fb"
            },
            # TODO: support it by client, move to options.
            "remote_copy_network": "fastbone",
            "version": 1
        },
        "plato": {
            "type": "yt",
            "options": {
                "proxy": "plato.yt.yandex.net",
                "hosts": "hosts/fb"
            },
            "remote_copy_network": "fastbone",
        },
        "cedar": {
            "type": "yamr",
            "options": {
                "server": "cedar00.search.yandex.net",
                "opts": "MR_NET_TABLE=ipv6",
                "binary": "/opt/cron/tools/mapreduce",
                "server_port": 8013,
                "http_port": 13013,
                "fastbone": True,
                "viewer": "https://specto.yandex.ru/cedar-viewer/"
            }
        },
        "redwood": {
            "type": "yamr",
            "options": {
                "server": "redwood00.search.yandex.net",
                "binary": "/opt/cron/tools/mapreduce",
                "server_port": 8013,
                "http_port": 13013,
                "fastbone": True,
                "viewer": "https://specto.yandex.ru/redwood-viewer/"
            }
        },
        "apterix": {
            "type": "kiwi",
            "options": {
                "url": "fb-kiwi1500.search.yandex.net",
                "kwworm": "/home/monster/kwworm"
            }
        }
    },
    "kiwi_transmitter": {
        "proxy": "flux.yt.yandex.net",
        "token": "93b4cacc08aa4538a79a76c21e99c0fb"
    },
    "availability_graph": {
        "kant": ["cedar", "redwood", "smith", "plato"],
        "smith": ["cedar", "redwood", "kant", "plato"],
        "plato": ["cedar", "redwood", "kant", "smith", "apterix"],
        "cedar": ["redwood", "kant", "smith", "plato"],
        "redwood": ["cedar", "kant", "smith", "plato"]
    },
    "default_mr_user": "userdata",
    "path": "//home/ignat/transfer_manager_test",
    "proxy": "kant.yt.yandex.net",
    "token": "93b4cacc08aa4538a79a76c21e99c0fb",
    "port": 5010
}

def main():
    parser = argparse.ArgumentParser(description="Transfer manager.")
    parser.add_argument("--config")
    args = parser.parse_args()

    if args.config is not None:
        config = json.load(open(args.config))
    else:
        config = DEFAULT_CONFIG

    app = Application(config)
    app.run(host=config.get("host", "::"), port=config["port"], debug=True, use_reloader=False, threaded=True)
    app.terminate()

if __name__ == "__main__":
    main()
