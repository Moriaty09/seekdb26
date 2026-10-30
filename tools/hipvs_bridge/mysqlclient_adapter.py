"""Small single-connection adapter for an already installed MySQL C client.

Used by local benchmarks when the image has libmysqlclient but no Python
connector. Does not install packages or implement the wire protocol.
"""
import ctypes as c
import ctypes.util


class Connection:
    def __init__(self, host, port, user, password, database, autocommit=True):
        path = ctypes.util.find_library('mysqlclient')
        if not path:
            raise RuntimeError('Existing libmysqlclient not found; use mysql.connector')
        self.lib = c.CDLL(path)
        signatures = {
            'mysql_init': (c.c_void_p, [c.c_void_p]),
            'mysql_real_connect': (c.c_void_p, [c.c_void_p, c.c_char_p, c.c_char_p,
                                              c.c_char_p, c.c_char_p, c.c_uint,
                                              c.c_char_p, c.c_ulong]),
            'mysql_autocommit': (c.c_bool, [c.c_void_p, c.c_bool]),
            'mysql_real_query': (c.c_int, [c.c_void_p, c.c_char_p, c.c_ulong]),
            'mysql_store_result': (c.c_void_p, [c.c_void_p]),
            'mysql_field_count': (c.c_uint, [c.c_void_p]),
            'mysql_num_fields': (c.c_uint, [c.c_void_p]),
            'mysql_fetch_row': (c.POINTER(c.c_char_p), [c.c_void_p]),
            'mysql_errno': (c.c_uint, [c.c_void_p]),
            'mysql_error': (c.c_char_p, [c.c_void_p]),
            'mysql_free_result': (None, [c.c_void_p]),
            'mysql_close': (None, [c.c_void_p]),
        }
        for name, (result, arguments) in signatures.items():
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = result, arguments
        self.handle = self.lib.mysql_init(None)
        self.active_cursor = None
        if not self.handle:
            raise RuntimeError('mysql_init failed')
        try:
            if not self.lib.mysql_real_connect(self.handle, host.encode(), user.encode(),
                                              password.encode(), database.encode(),
                                              port, None, 0):
                self.fail()
            if self.lib.mysql_autocommit(self.handle, int(autocommit)):
                self.fail()
        except Exception:
            self.close()
            raise

    def fail(self):
        number = self.lib.mysql_errno(self.handle)
        error = self.lib.mysql_error(self.handle).decode('utf-8', errors='replace')
        raise RuntimeError(f'MySQL error {number}: {error}')

    def cursor(self):
        if self.active_cursor:
            raise RuntimeError('Adapter supports one active cursor')
        self.active_cursor = Cursor(self)
        return self.active_cursor

    def close(self):
        if self.active_cursor:
            self.active_cursor.close()
        if self.handle:
            self.lib.mysql_close(self.handle)
            self.handle = None


class Cursor:
    def __init__(self, connection):
        self.connection = connection
        self.result = None

    def execute(self, sql):
        lib, handle = self.connection.lib, self.connection.handle
        if self.result:
            lib.mysql_free_result(self.result)
            self.result = None
        encoded = sql.encode('utf-8')
        if lib.mysql_real_query(handle, encoded, len(encoded)):
            self.connection.fail()
        self.result = lib.mysql_store_result(handle)
        if not self.result and lib.mysql_field_count(handle):
            self.connection.fail()

    def fetchall(self):
        if not self.result:
            return []
        lib = self.connection.lib
        columns = lib.mysql_num_fields(self.result)
        rows = []
        while True:
            row = lib.mysql_fetch_row(self.result)
            if not row:
                break
            # SQL benchmark returns integer IDs; bytes preserve exact decimal IDs.
            rows.append(tuple(row[i] for i in range(columns)))
        if lib.mysql_errno(self.connection.handle):
            self.connection.fail()
        lib.mysql_free_result(self.result)
        self.result = None
        return rows

    def close(self):
        if self.result:
            self.connection.lib.mysql_free_result(self.result)
            self.result = None
        self.connection.active_cursor = None
