
#
# This python script ensures that gdb prints XDR types properly.  To
# use it, place the following in your .gdbinit file:
#
# source /path/to/x23/x32-gdb.py
#

import gdb
import re

class UnionPrinter:
    "Print union types compiled by x23"

    __fieldprefix = "__xdr_union_field_"

    def __init__(self, val):
        self.__val = val
        fields = val.type.fields()
        self.__fields = [fields[0].name]
        if len(fields) > 1:
            for f in fields[1].type.fields():
                self.__fields.append(f.name)

    def get(self, fieldno):
        return self.__val[self.__fields[fieldno]]

    def active_arm(self):
        # This assumes the __xdr_union_arm_number static method
        # doesn't get optimized away, which can be ensured via the
        # [[gnu::used]] attribute.
        discriminant = self.get(0);
        arm_expr = "{}::__xdr_union_arm_number({})".format(
            self.__val.type.name, str(discriminant))
        try:
            return int(gdb.parse_and_eval(arm_expr))
        except gdb.error as e:
            print(str(e))
            return -1

    def children(self):
        result = [(self.__fields[0].removeprefix(self.__fieldprefix),
                   self.get(0))]
        n = self.active_arm()
        if n > 0:
            result.append((self.__fields[n].removeprefix(self.__fieldprefix),
                           self.get(n)))
        elif n == -1:
            result.append(("<no arm for invalid discriminant>", ""))
        return result

class StrPrinter:
    "Print x23::Str, and x23::BoundedStr types"

    def __init__(self, val):
        self.__val = val

    def to_string(self):
        s = self.__val['b_']['begin']
        l = self.__val['b_']['end'] - s
        return s.lazy_string(length = l)

    def display_hint(self):
        return 'string'

class VecPrinter:
    "Print x23::Vec types"

    def __init__(self, val):
        self.__val = val

    def to_string(self):
        begin = self.__val['b_']['begin']
        end = self.__val['b_']['end']
        length = int(end-begin)
        return f"x23::Vec of length {length}"

    def display_hint(self):
        return 'array'

    def children(self):
        begin = self.__val['b_']['begin']
        end = self.__val['b_']['end']
        count = 0

        while begin != end:
            yield f'[{count}]', begin.dereference()
            begin = begin + 1
            count = count + 1

class Constants:
    "A cache for constant values, such as enum tags"

    def __init__(self, prefix = ""):
        self._prefix = prefix
        self._cache = {}

    def __getitem__(self, symbol):
        if symbol not in self._cache:
            try:
                self._cache[symbol] = gdb.parse_and_eval(self._prefix + symbol)
            except gdb.error:
                return None
        return self._cache[symbol]

categories = Constants("x23::XdrCategory::")

def xdr_category(val):
    try:
        return gdb.parse_and_eval(
            f"x23::xdr_meta<{val.type.strip_typedefs().name}>::kCategory")
    except gdb.error:
        return None

def x23_lookup_function(val):
    typename = val.type.strip_typedefs().name
    if re.match(r'^x23::BoundedStr<', typename):
        return StrPrinter(val)
    elif re.match(r'^x23::Vec<', typename):
        return VecPrinter(val)
    cat = xdr_category(val)
    if cat == None:
        return None
    elif cat == categories["kUnion"]:
        return UnionPrinter(val)
    else:
        return None

gdb.printing.register_pretty_printer(gdb.current_objfile(), x23_lookup_function)
