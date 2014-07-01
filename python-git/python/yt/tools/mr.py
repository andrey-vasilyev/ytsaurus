import yt.logger as logger

import sh
import subprocess
import simplejson as json
from urllib import quote_plus

class Mr(object):
    def __init__(self, binary, server, server_port, http_port, proxies=None, proxy_port=None, fetch_info_from_http=False, mr_user="userdata", fastbone=False, opts=""):
        self.binary = binary
        self.server = self._make_address(server, server_port)
        self.http_server = self._make_address(server, http_port)
        if proxies is None:
            self.proxies = []
        else:
            self.proxies = map(lambda proxy: self._make_address(proxy, proxy_port), proxies)
        self.fetch_info_from_http = fetch_info_from_http

        self.cache = {}
        self.mr_user = mr_user
        self.opts = opts
        self.fastbone = fastbone

        self.supports_shared_transactions = \
            subprocess.call("{0} --help | grep sharedtransaction &>/dev/null".format(self.binary), shell=True) == 0

        logger.info("Yamr options configured (binary: %s, server: %s, http_server: %s, proxies: [%s])",
                    self.binary, self.server, self.http_server, ", ".join(self.proxies))

    
    def _make_address(self, address, port):
        if ":" in address and address.rsplit(":", 1)[1].isdigit():
            address = address.rsplit(":", 1)[0]
        return "{0}:{1}".format(address, port)


    def get_field_from_page(self, table, field):
        """ Extract value of given field from http page of the table """
        http_content = sh.curl("{0}/debug?info=table&table={1}".format(self.http_server, table)).stdout
        records_line = filter(lambda line: line.find(field) != -1,  http_content.split("\n"))[0]
        records_line = records_line.replace("</b>", "").replace("<br>", "").replace("<b>", "").replace(",", "")
        return records_line.split(field + ":")[1].split()[0]

    def get_field_from_server(self, table, field, allow_cache):
        if not allow_cache or table not in self.cache:
            output = subprocess.check_output(
                "{0} -server {1} -list -prefix {2} -jsonoutput"\
                    .format(self.binary, self.server, table),
                shell=True)
            table_info = filter(lambda obj: obj["name"] == table, json.loads(output))
            if table_info:
                self.cache[table] = table_info[0]
            else:
                self.cache[table] = None
        
        if self.cache[table] is None:
            return None
        
        return self.cache[table].get(field, None)

    def records_count(self, table, allow_cache=False):
        if self.fetch_info_from_http:
            return int(self.get_field_from_page(table, "Records"))
        else:
            return self._as_int(self.get_field_from_server(table, "records", allow_cache=allow_cache))
    
    def data_size(self, table, allow_cache=False):
        if self.fetch_info_from_http:
            return int(self.get_field_from_page(table, "Size"))
        else:
            return self._as_int(self.get_field_from_server(table, "size", allow_cache=allow_cache))

    def is_sorted(self, table, allow_cache=False):
        if self.fetch_info_from_http:
            return self.get_field_from_page(table, "Sorted").lower() == "yes"
        else:
            return self.get_field_from_server(table, "sorted", allow_cache=allow_cache) == 1
    
    def is_empty(self, table, allow_cache=False):
        if not self.fetch_info_from_http:
            count = self.get_field_from_server(table, "records", allow_cache=allow_cache)
            return count is None or count == 0
        """ Parse whether table is empty from html """
        http_content = sh.curl("{0}/debug?info=table&table={1}".format(self.http_server, table)).stdout
        empty_lines = filter(lambda line: line.find("is empty") != -1,  http_content.split("\n"))
        return empty_lines and empty_lines[0].startswith("Table is empty")

    def drop(self, table):
        subprocess.check_call("USER=yt MR_USER={0} {1} -server {2} -drop {3}".format(self.mr_user, self.binary, self.server, table), shell=True)

    def copy(self, src, dst):
        subprocess.check_call("USER=yt MR_USER={0} {1} -server {2} -copy -src {3} -dst {4}".format(self.mr_user, self.binary, self.server, src, dst), shell=True)

    def _as_int(self, obj):
        if obj is None:
            return 0
        return int(obj)

    def get_read_range_command(self, table):
        if self.proxies:
            return 'curl "http://${{server}}/table/{0}?subkey=1&lenval=1&startindex=${{start}}&endindex=${{end}}"'.format(quote_plus(table))
        else:
            fastbone_str = "-opt net_table=fastbone" if self.fastbone else ""
            shared_tx_str = "-sharedtransactionid yt" if self.supports_shared_transactions else ""
            return '{0} USER=yt MR_USER={1} ./mapreduce -server $server {2} -read {3}:[$start,$end] -lenval -subkey {4}'\
                        .format(self.opts, self.mr_user, fastbone_str, table, shared_tx_str)

    def get_write_command(self, table):
        fastbone_str = "-opt net_table=fastbone" if self.fastbone else ""
        return "{0} USER=yt MR_USER={1} ./mapreduce -server {2} {3} -append -lenval -subkey -write {4}"\
                .format(self.opts, self.mr_user, self.server, fastbone_str, table)
