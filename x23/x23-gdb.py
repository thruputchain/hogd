import gdb
import re

class UnionPrinter:
    "Print union types compiled by x23"

    fieldprefix = "__xdr_union_field_"

    def __init__(self, val):
        self.val = val
        fields = val.type.fields()
        self.fields = [fields[0].name]
        if len(fields) > 1:
            for f in fields[1].type.fields():
                self.fields.append(f.name)

    def get(self, fieldno):
        return self.val[self.fields[fieldno]]

    def active_arm(self):
         expression = '({}::__xdr_union_arm_number({}))'.format(
             self.val.type.name, str(self.get(0)))
         try:
             return int(gdb.parse_and_eval(expression))
         except gdb.error:
             return -1

    def to_string(self):
        return None

    def children(self):
        result = [(self.fields[0].removeprefix(self.fieldprefix), self.get(0))]
        n = self.active_arm()
        if n > 0:
            result.append((self.fields[n].removeprefix(self.fieldprefix),
                           self.get(n)))
        return result

class StrPrinter:
    "Print x23::Str"

    def __init__ (self, val):
        self.val = val

    def to_string (self):
        s = self.val['b_']['begin']
        l = self.val['b_']['end'] - s
        return s.lazy_string(length = l)

    def display_hint(self):
        return 'string'

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
            f"x23::xdr_meta<{val.type.strip_typedefs().name}>::category")
    except gdb.error:
        return None

def lookup_function(val):
    if re.match(r'^x23::BoundedStr<', val.type.strip_typedefs().name):
        return StrPrinter(val)
    cat = xdr_category(val)
    if cat == None:
        return None
    elif cat == categories["kUnion"]:
        return UnionPrinter(val)
    else:
        return None

gdb.printing.register_pretty_printer(gdb.current_objfile(), lookup_function)
