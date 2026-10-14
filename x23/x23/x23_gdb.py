
#
# This python script ensures that gdb prints XDR types properly.  To
# use it, place the following in your .gdbinit file:
#
# source /path/to/x23/x32-gdb.py
#

import gdb
import re


def _strip_refs(gdb_type):
    ref_codes = []
    for attr in ('TYPE_CODE_REF', 'TYPE_CODE_RVALUE_REF'):
        code = getattr(gdb, attr, None)
        if code is not None:
            ref_codes.append(code)
    try:
        while True:
            code = gdb_type.code
            if ref_codes and code in ref_codes:
                gdb_type = gdb_type.target()
                continue
            break
    except gdb.error:
        pass
    return gdb_type


def _safe_type_name(gdb_type):
    gdb_type = _strip_refs(gdb_type)
    try:
        stripped = gdb_type.strip_typedefs()
    except gdb.error:
        stripped = gdb_type
    for attr in ('name', 'tag'):
        try:
            name = getattr(stripped, attr)
            if name:
                return _normalize_type_name(name)
        except gdb.error:
            continue
    try:
        return _normalize_type_name(str(stripped))
    except Exception:
        return ""


def _normalize_type_name(name):
    if not name:
        return ""
    prefixes = ('struct ', 'class ', 'union ')
    for prefix in prefixes:
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    name = name.strip()
    suffixes = (' const', ' volatile', ' &', ' &&')
    removed = True
    while removed and name:
        removed = False
        for suffix in suffixes:
            if name.endswith(suffix):
                name = name[: -len(suffix)]
                name = name.rstrip()
                removed = True
                break
    return name

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

class PtrPrinter:
    "Print x23::Ptr types by exposing the raw pointer"

    def __init__(self, val):
        self.__val = val

    def _element_type(self):
        try:
            base_type = _strip_refs(self.__val.type)
            stripped = base_type.strip_typedefs()
            tmpl_arg = stripped.template_argument(0)
            return tmpl_arg
        except (gdb.error, IndexError, AttributeError, RuntimeError, TypeError):
            return None

    def _raw_ptr(self):
        val = self.__val
        objs = [val]
        try:
            stripped_type = val.type.strip_typedefs()
            address = val.address
            for field in stripped_type.fields():
                if getattr(field, 'is_base_class', False):
                    try:
                        if address:
                            base = address.cast(field.type.pointer()).dereference()
                        else:
                            base = val.cast(field.type)
                    except gdb.error:
                        continue
                    objs.append(base)
        except gdb.error:
            pass
        accessors = (
            lambda v: v['ptr_'],
            lambda v: v['__ptr_'],  # libc++
            lambda v: v['_M_ptr'],  # libstdc++ pre-C++11 ABI
            lambda v: v['_M_t']['_M_t']['_M_head_impl'],  # libstdc++ common
            lambda v: v['__data_']['__value_'],
        )
        for obj in objs:
            for accessor in accessors:
                try:
                    ptr = accessor(obj)
                    if ptr.type.code == gdb.TYPE_CODE_PTR:
                        return ptr
                except (gdb.error, RuntimeError, ValueError, TypeError, KeyError):
                    continue
        return None

    def _resolved_ptr(self):
        ptr = self._raw_ptr()
        if ptr is None:
            ptr = self._call_get()
        return ptr

    def to_string(self):
        ptr = self._resolved_ptr()
        if ptr is None:
            return '<invalid x23::Ptr>'
        if self._is_null(ptr):
            return 'NULL'
        elem_type = self._element_type()
        if elem_type is not None:
            try:
                deref = ptr.cast(elem_type.pointer()).dereference()
                value_str = str(deref)
                trimmed = value_str.lstrip()
                if trimmed.startswith('{'):
                    return trimmed
                return '{' + value_str + '}'
            except gdb.error:
                pass
        try:
            void_ptr = ptr.cast(gdb.lookup_type('void').pointer())
        except gdb.error:
            void_ptr = ptr
        return str(void_ptr)

    def display_hint(self):
        return 'pointer'

    def _call_get(self):
        try:
            addr = self.__val.address
        except gdb.error:
            return None
        if addr is None:
            return None
        try:
            type_name = _safe_type_name(self.__val.type)
            if not type_name:
                raise gdb.error('')
            addr_str = str(addr)
            expr = f"(({type_name}*){addr_str})->get()"
            ptr = gdb.parse_and_eval(expr)
            if ptr.type.code == gdb.TYPE_CODE_PTR:
                return ptr
        except gdb.error:
            pass
        try:
            void_ptr_type = gdb.lookup_type('void').pointer()
            ptr_ptr = addr.cast(void_ptr_type.pointer())
            ptr = ptr_ptr.dereference()
            if ptr.type.code == gdb.TYPE_CODE_PTR:
                return ptr
        except gdb.error:
            pass
        return None

    def _is_null(self, ptr):
        try:
            if ptr == 0:
                return True
        except gdb.error:
            pass
        try:
            return int(ptr) == 0
        except (TypeError, ValueError, gdb.error):
            return False

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
        type_name = _safe_type_name(val.type)
        if not type_name:
            return None
        return gdb.parse_and_eval(
            f"x23::xdr_meta<{type_name}>::category")
    except gdb.error:
        return None

def x23_lookup_function(val):
    typename = _safe_type_name(val.type)
    if not typename:
        return None
    try:
        if re.match(r'^x23::BoundedStr<', typename):
            return StrPrinter(val)
        elif re.match(r'^x23::Vec<', typename):
            return VecPrinter(val)
        elif re.match(r'^x23::Ptr<', typename):
            return PtrPrinter(val)
    except TypeError:
        return None
    cat = xdr_category(val)
    if cat == None:
        return None
    elif cat == categories["kUnion"]:
        return UnionPrinter(val)
    else:
        return None

gdb.printing.register_pretty_printer(gdb.current_objfile(), x23_lookup_function)
