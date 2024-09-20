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
        discriminant = self.get(0);
        cases_expr = "x23::xdr_meta<{}>::cases".format(self.__val.type.name)
        try:
            cases = gdb.parse_and_eval(cases_expr)
            # Need to convert std::array to C array
            cases = cases[cases.type.fields()[0].name]
            for i in cases.type.strip_typedefs().range():
                if cases[i]['first'] == discriminant:
                    return int(cases[i]['second'])
            def_expr = "x23::xdr_meta<{}>::default_field".format(
                self.__val.type.name)
            return int(gdb.parse_and_eval(def_expr))
        except gdb.error as e:
            print(str(e))
            return -1

    def to_string(self):
        return None

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
    "Print x23::Str"

    def __init__ (self, val):
        self.__val = val

    def to_string (self):
        s = self.__val['b_']['begin']
        l = self.__val['b_']['end'] - s
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

def x23_lookup_function(val):
    if re.match(r'^x23::BoundedStr<', val.type.strip_typedefs().name):
        return StrPrinter(val)
    cat = xdr_category(val)
    if cat == None:
        return None
    elif cat == categories["kUnion"]:
        return UnionPrinter(val)
    else:
        return None

gdb.printing.register_pretty_printer(gdb.current_objfile(), x23_lookup_function)
