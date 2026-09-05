"""
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
"""

import re
import os
import pathlib
from typing import Any, Tuple

# Built-in TL types that are not serialized as constructors.
_BUILTINS = {
    "int": "ReadInt(from, end)",
    "long": "ReadLong(from, end)",
    "double": "ReadDouble(from, end)",
    "string": "ReadString(from, end)",
    "bytes": "ReadBytesValue(from, end)",
    "int128": "ReadWideBytes(from, end, 4)",
    "int256": "ReadWideBytes(from, end, 8)",
}

# The same built-in types, written from JSON values.
_BUILTIN_WRITERS = {
    "int": "WriteInt",
    "long": "WriteLong",
    "double": "WriteDoubleValue",
    "string": "WriteStringValue",
    "bytes": "WriteBytesValue",
    "int128": "WriteWideBytesValue",
    "int256": "WriteWideBytesValue",
}

# TL constructors that carry their payload as plain JSON scalars.
_BOOL_TRUE = 0x997275B5
_BOOL_FALSE = 0xBC799737
_VECTOR = 0x1CB5C415

_LINE_RE = re.compile(r"^([a-zA-Z0-9_.]+)(#[0-9a-f]+)?\s*([^=]*?)\s*=\s*([^;]+);\s*$")
_PARAM_RE = re.compile(r"^([a-zA-Z_][a-zA-Z0-9_]*):(.+)$")
_CONDITION_RE = re.compile(r"^([a-z_][a-z0-9_]*)\.([0-9]+)\?(.+)$")
_VECTOR_RE = re.compile(r"^[vV]ector<(.+)>$")


class Constructor:
    def __init__(self, name, cons_id, fields):
        self.name = name
        self.id = cons_id
        self.fields = fields

    @property
    def cpp_name(self):
        return self.name.replace(".", "_")


def bare_key(name):
    # TL bare type reference: CamelCase -> snake_case.
    last = name.split(".")[-1]
    return re.sub(r"(?<!^)(?=[A-Z])", "_", last).lower()


def parse(input_files):
    constructors = []
    functions = []
    types = {}
    for path in input_files:
        section_types = True
        with open(path, encoding="utf-8") as f:
            for line in f:
                comment = re.match(r"^(.*?)//", line)
                line = comment.group(1) if comment else line
                if re.match(r"\s*---functions---", line):
                    section_types = False
                    continue
                elif re.match(r"\s*---types---", line):
                    section_types = True
                    continue
                elif re.match(r"^\s*$", line):
                    continue
                match = _LINE_RE.match(line)
                if not match:
                    raise ValueError("Bad line: " + line)
                cons_hex = match.group(2)
                if not cons_hex:
                    # Built-in type declarations like "int ? = Int;".
                    continue
                if re.search(r"\{[a-zA-Z_][a-zA-Z0-9_]*:Type\}", match.group(3)):
                    # Generic (templated) declarations like the
                    # invokeWithLayer wrappers: no fixed wire form.
                    continue
                fields = []
                for param in match.group(3).split():
                    pair = _PARAM_RE.match(param)
                    if not pair:
                        raise ValueError("Bad param: " + param)
                    pname = pair.group(1)
                    ptype = pair.group(2)
                    cond = None
                    masked = _CONDITION_RE.match(ptype)
                    if masked:
                        cond = (masked.group(1), int(masked.group(2)))
                        ptype = masked.group(3)
                    fields.append((pname, ptype, cond))
                constructor = Constructor(match.group(1), int(cons_hex[1:], 16), fields)
                if section_types:
                    constructors.append(constructor)
                    types.setdefault(match.group(4), []).append(constructor)
                else:
                    functions.append(constructor)
    return constructors, functions, types


def bare_map(types):
    result = {}
    for name in types:
        key = bare_key(name)
        if key in result:
            # Ambiguous bare reference: no bare fields may use it.
            result[key] = None
        else:
            result[key] = name
    return result


def classify(ptype, types, bares):
    vector = _VECTOR_RE.match(ptype)
    if vector:
        return ("vector", classify(vector.group(1), types, bares))
    elif ptype in _BUILTINS:
        return ("builtin", ptype)
    elif ptype in ("True", "true"):
        return ("true", None)
    elif ptype == "Bool":
        return ("bool", None)
    elif ptype in bares:
        return ("bare", bares[ptype])
    elif ptype == "X" or ptype in types:
        return ("boxed", ptype)
    raise ValueError("Bad type: " + ptype)


def check_fields(constructor, types, bares):
    flags_seen = set()
    for pname, ptype, cond in constructor.fields:
        if ptype == "#":
            flags_seen.add(pname)
            continue
        if cond is not None and cond[0] not in flags_seen:
            raise ValueError(
                "Conditional before its flags field: " + constructor.name + ":" + pname
            )
        kind = classify(ptype, types, bares)[0]
        if kind == "bare" and len(types[bares[ptype]]) != 1:
            raise ValueError(
                "Bare type is not a single constructor: "
                + constructor.name
                + ":"
                + pname
            )


_HEADER_TEMPLATE = """\
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/core_types.h"

#include <QtCore/QJsonObject>

namespace MTP::details {

// Decodes a single boxed TL object starting with its constructor id into
// a complete recursive JSON representation: every constructor becomes an
// object with a "_" key holding its TL name, every field present on the
// wire becomes a JSON key, flag-disabled optional fields are absent.
[[nodiscard]] bool TlJsonDecodeBoxed(
\tQJsonObject &to,
\tconst mtpPrime *&from,
\tconst mtpPrime *end);

// The TL name of the boxed constructor id at |from|, or an empty string.
[[nodiscard]] QString TlJsonBoxedName(
\tconst mtpPrime *from,
\tconst mtpPrime *end);

// Encodes a single boxed TL object from its complete recursive JSON
// representation: the "_" key selects the constructor (types and
// functions alike), required fields must be present, conditional fields
// are written only when their key is present and their flag bits are
// computed from that presence unless the flags field is given
// explicitly. Unknown keys are ignored, malformed values fail.
[[nodiscard]] bool TlJsonEncodeBoxed(
\tconst QJsonObject &from,
\tmtpBuffer &to);

} // namespace MTP::details
"""


_CPP_TEMPLATE = """\
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "@include_name~"

#include <QtEndian>

#include <QtCore/QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace MTP::details {

namespace {

constexpr auto kBoolTrueConstructor = 0x@bool_true~U;
constexpr auto kBoolFalseConstructor = 0x@bool_false~U;
constexpr auto kVectorConstructor = 0x@vector~U;

[[nodiscard]] bool Has(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end,
\t\tint count) {
\treturn (end - from) >= count;
}

[[nodiscard]] std::optional<QByteArray> ReadTlBytes(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tif (!Has(from, end, 1)) {
\t\treturn std::nullopt;
\t}
\tconst auto first = quint32(*from++);
\tconst auto longHeader = ((first & 0xFFU) == 0xFEU);
\tconst auto length = longHeader
\t\t? int((first >> 8) & 0xFFFFFFU)
\t\t: int(first & 0xFFU);
\tconst auto header = longHeader ? 4 : 1;
\tconst auto primes = (header + length + 3) / 4;
\tif (!Has(from, end, primes - 1)) {
\t\treturn std::nullopt;
\t}
\tauto buffer = QByteArray(primes * 4, Qt::Uninitialized);
\tauto out = reinterpret_cast<uchar *>(buffer.data());
\tqToLittleEndian(first, out);
\tout += 4;
\tfor (auto i = 0; i != primes - 1; ++i) {
\t\tqToLittleEndian(quint32(*from++), out);
\t\tout += 4;
\t}
\treturn buffer.mid(header, length);
}

[[nodiscard]] std::optional<QJsonValue> ReadInt(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tif (!Has(from, end, 1)) {
\t\treturn std::nullopt;
\t}
\treturn QJsonValue(double(int32(quint32(*from++))));
}

[[nodiscard]] std::optional<QJsonValue> ReadLong(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tif (!Has(from, end, 2)) {
\t\treturn std::nullopt;
\t}
\tconst auto low = quint32(*from++);
\tconst auto high = quint32(*from++);
\tconst auto value = (quint64(high) << 32) | low;
\t// int64 is exact in double only below 2^53, otherwise it is emitted
\t// as a string to preserve the exact numeric value.
\tif (value >> 53) {
\t\treturn QJsonValue(QString::number(static_cast<qint64>(value)));
\t}
\treturn QJsonValue(double(static_cast<qint64>(value)));
}

[[nodiscard]] std::optional<QJsonValue> ReadDouble(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tif (!Has(from, end, 2)) {
\t\treturn std::nullopt;
\t}
\tconst auto low = quint32(*from++);
\tconst auto high = quint32(*from++);
\tconst auto bits = (quint64(high) << 32) | low;
\tauto value = double();
\tmemcpy(&value, &bits, sizeof(value));
\treturn QJsonValue(value);
}

[[nodiscard]] std::optional<QJsonValue> ReadWideBytes(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end,
\t\tint primes) {
\tif (!Has(from, end, primes)) {
\t\treturn std::nullopt;
\t}
\tauto buffer = QByteArray(primes * 4, Qt::Uninitialized);
\tauto out = reinterpret_cast<uchar *>(buffer.data());
\tfor (auto i = 0; i != primes; ++i) {
\t\tqToLittleEndian(quint32(*from++), out);
\t\tout += 4;
\t}
\treturn QJsonValue(QString::fromLatin1(buffer.toBase64()));
}

[[nodiscard]] std::optional<QJsonValue> ReadString(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tconst auto bytes = ReadTlBytes(from, end);
\tif (!bytes) {
\t\treturn std::nullopt;
\t}
\treturn QJsonValue(QString::fromUtf8(*bytes));
}

[[nodiscard]] std::optional<QJsonValue> ReadBytesValue(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tconst auto bytes = ReadTlBytes(from, end);
\tif (!bytes) {
\t\treturn std::nullopt;
\t}
\treturn QJsonValue(QString::fromLatin1(bytes->toBase64()));
}

[[nodiscard]] std::optional<QJsonValue> ReadBool(
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tif (!Has(from, end, 1)) {
\t\treturn std::nullopt;
\t}
\tconst auto cons = quint32(*from++);
\tif (cons == kBoolTrueConstructor) {
\t\treturn QJsonValue(true);
\t} else if (cons == kBoolFalseConstructor) {
\t\treturn QJsonValue(false);
\t}
\treturn std::nullopt;
}

bool Insert(
\t\tQJsonObject &obj,
\t\tQLatin1String key,
\t\tconst std::optional<QJsonValue> &value) {
\tif (!value) {
\t\treturn false;
\t}
\tobj.insert(key, *value);
\treturn true;
}

bool Append(QJsonArray &list, const std::optional<QJsonValue> &value) {
\tif (!value) {
\t\treturn false;
\t}
\tlist.append(*value);
\treturn true;
}

void PushTlBytes(const QByteArray &bytes, mtpBuffer &to) {
\tconst auto length = int(bytes.size());
\tauto full = QByteArray();
\tif (length < 0xFE) {
\t\tfull.append(char(uchar(length)));
\t} else {
\t\tfull.append(char(0xFE));
\t\tfull.append(char(length & 0xFF));
\t\tfull.append(char((length >> 8) & 0xFF));
\t\tfull.append(char((length >> 16) & 0xFF));
\t}
\tfull.append(bytes);
\twhile (full.size() & 0x03) {
\t\tfull.append(char(0));
\t}
\tconst auto primes = full.size() / 4;
\tconst auto data = reinterpret_cast<const uchar *>(full.constData());
\tfor (auto i = 0; i != primes; ++i) {
\t\tquint32 value = 0;
\t\tmemcpy(&value, data + i * 4, 4);
\t\tto.push_back(mtpPrime(qFromLittleEndian(value)));
\t}
}

[[nodiscard]] bool WriteInt(const QJsonValue &value, mtpBuffer &to) {
\tauto result = int32(0);
\tif (value.isDouble()) {
\t\tconst auto d = value.toDouble();
\t\tif (d != std::floor(d)
\t\t\t|| d < double(-2147483648LL)
\t\t\t|| d > double(2147483647LL)) {
\t\t\treturn false;
\t\t}
\t\tresult = int32(d);
\t} else if (value.isString()) {
\t\tauto ok = false;
\t\tconst auto v = value.toString().toLongLong(&ok);
\t\tif (!ok || v < -2147483648LL || v > 2147483647LL) {
\t\t\treturn false;
\t\t}
\t\tresult = int32(v);
\t} else {
\t\treturn false;
\t}
\tto.push_back(mtpPrime(result));
\treturn true;
}

[[nodiscard]] bool WriteLong(const QJsonValue &value, mtpBuffer &to) {
\tauto result = qint64(0);
\tif (value.isDouble()) {
\t\tconst auto d = value.toDouble();
\t\tif (d != std::floor(d)
\t\t\t|| d < -9007199254740992.0
\t\t\t|| d > 9007199254740992.0) {
\t\t\treturn false;
\t\t}
\t\tresult = qint64(d);
\t} else if (value.isString()) {
\t\tauto ok = false;
\t\tresult = value.toString().toLongLong(&ok);
\t\tif (!ok) {
\t\t\treturn false;
\t\t}
\t} else {
\t\treturn false;
\t}
\tconst auto u = quint64(result);
\tto.push_back(mtpPrime(quint32(u & 0xFFFFFFFFU)));
\tto.push_back(mtpPrime(quint32(u >> 32)));
\treturn true;
}

[[nodiscard]] bool WriteDoubleValue(const QJsonValue &value, mtpBuffer &to) {
\tif (!value.isDouble()) {
\t\treturn false;
\t}
\tconst auto d = value.toDouble();
\tquint32 parts[2];
\tmemcpy(parts, &d, sizeof(parts));
\tto.push_back(mtpPrime(parts[0]));
\tto.push_back(mtpPrime(parts[1]));
\treturn true;
}

[[nodiscard]] bool WriteStringValue(const QJsonValue &value, mtpBuffer &to) {
\tif (!value.isString()) {
\t\treturn false;
\t}
\tPushTlBytes(value.toString().toUtf8(), to);
\treturn true;
}

[[nodiscard]] bool WriteBytesValue(const QJsonValue &value, mtpBuffer &to) {
\tif (!value.isString()) {
\t\treturn false;
\t}
\tconst auto decoded = QByteArray::fromBase64Encoding(
\t\tvalue.toString().toLatin1(),
\t\tQByteArray::Base64Encoding
\t\t\t| QByteArray::AbortOnBase64DecodingErrors);
\tif (!decoded) {
\t\treturn false;
\t}
\tPushTlBytes(*decoded, to);
\treturn true;
}

[[nodiscard]] bool WriteWideBytesValue(
\t\tconst QJsonValue &value,
\t\tint primes,
\t\tmtpBuffer &to) {
\tif (!value.isString()) {
\t\treturn false;
\t}
\tconst auto decoded = QByteArray::fromBase64Encoding(
\t\tvalue.toString().toLatin1(),
\t\tQByteArray::Base64Encoding
\t\t\t| QByteArray::AbortOnBase64DecodingErrors);
\tif (!decoded || (*decoded).size() != primes * 4) {
\t\treturn false;
\t}
\tconst auto data = reinterpret_cast<const uchar *>((*decoded).constData());
\tfor (auto i = 0; i != primes; ++i) {
\t\tquint32 v = 0;
\t\tmemcpy(&v, data + i * 4, 4);
\t\tto.push_back(mtpPrime(qFromLittleEndian(v)));
\t}
\treturn true;
}

[[nodiscard]] bool WriteBoolValue(const QJsonValue &value, mtpBuffer &to) {
\tif (!value.isBool()) {
\t\treturn false;
\t}
\tto.push_back(mtpPrime(value.toBool()
\t\t? mtpPrime(kBoolTrueConstructor)
\t\t: mtpPrime(kBoolFalseConstructor)));
\treturn true;
}

struct TlJsonEntry {
\tquint32 cons;
\tconst char *name;
\tbool (*read)(QJsonObject &obj, const mtpPrime *&from, const mtpPrime *end);
};

struct TlJsonWriteEntry {
\tquint32 cons;
\tconst char *name;
\tbool (*write)(const QJsonObject &obj, mtpBuffer &to);
};

bool Cons(
\t\tQJsonObject &obj,
\t\tquint32 cons,
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end);

bool BoxedValue(QJsonObject &obj, const mtpPrime *&from, const mtpPrime *end);

bool Boxed(
\t\tQJsonObject &obj,
\t\tQLatin1String key,
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end);

@forward_declarations~
@constructor_functions~
constexpr TlJsonEntry kEntries[] = {
@table_entries~
};

@write_forward_declarations~
@write_functions~
constexpr TlJsonWriteEntry kWriteEntries[] = {
@write_table_entries~
};

const TlJsonEntry *LookupEntry(quint32 cons) {
\tauto i = std::lower_bound(
\t\tstd::begin(kEntries),
\t\tstd::end(kEntries),
\t\tcons,
\t\t[](const TlJsonEntry &entry, quint32 cons) {
\t\t\treturn entry.cons < cons;
\t\t});
\tif (i == std::end(kEntries) || i->cons != cons) {
\t\treturn nullptr;
\t}
\treturn i;
}

bool Cons(
\t\tQJsonObject &obj,
\t\tquint32 cons,
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tconst auto entry = LookupEntry(cons);
\tif (!entry) {
\t\treturn false;
\t}
\tobj.insert(QStringLiteral("_"), QString::fromUtf8(entry->name));
\treturn entry->read(obj, from, end);
}

bool BoxedValue(QJsonObject &obj, const mtpPrime *&from, const mtpPrime *end) {
\tif (!Has(from, end, 1)) {
\t\treturn false;
\t}
\tconst auto cons = quint32(*from++);
\treturn Cons(obj, cons, from, end);
}

bool Boxed(
\t\tQJsonObject &obj,
\t\tQLatin1String key,
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\tauto child = QJsonObject();
\tif (!BoxedValue(child, from, end)) {
\t\treturn false;
\t}
\tobj.insert(key, child);
\treturn true;
}

} // namespace

bool TlJsonDecodeBoxed(
\t\tQJsonObject &to,
\t\tconst mtpPrime *&from,
\t\tconst mtpPrime *end) {
\treturn BoxedValue(to, from, end);
}

QString TlJsonBoxedName(const mtpPrime *from, const mtpPrime *end) {
\tif (from == end) {
\t\treturn QString();
\t}
\tconst auto entry = LookupEntry(quint32(*from));
\treturn entry ? QString::fromUtf8(entry->name) : QString();
}

bool TlJsonEncodeBoxed(const QJsonObject &from, mtpBuffer &to) {
\tconst auto nameValue = from.value(QStringLiteral("_"));
\tif (!nameValue.isString()) {
\t\treturn false;
\t}
\tconst auto &name = nameValue.toString();
\tauto i = std::lower_bound(
\t\tstd::begin(kWriteEntries),
\t\tstd::end(kWriteEntries),
\t\tname,
\t\t[](const TlJsonWriteEntry &entry, const QString &name) {
\t\t\treturn name.compare(QLatin1String(entry.name)) > 0;
\t\t});
\tif (i == std::end(kWriteEntries) || name != QLatin1String(i->name)) {
\t\treturn false;
\t}
\tto.push_back(mtpPrime(i->cons));
\treturn i->write(from, to);
}

} // namespace MTP::details
"""


def sub(text, **args):
    for key, value in args.items():
        text = text.replace("@" + key + "~", str(value))
    return text


def generate(input_files, output_path):
    constructors, functions, types = parse(input_files)
    bares = bare_map(types)
    for constructor in constructors + functions:
        check_fields(constructor, types, bares)
    everything = constructors + functions

    functions_code = "".join(
        emit_constructor(constructor, types, bares) for constructor in everything
    )
    forward_declarations = "".join(
        sub(
            "bool TlJson_@cpp_name~(QJsonObject &obj, const mtpPrime *&from, "
            "const mtpPrime *end);\n",
            cpp_name=constructor.cpp_name,
        )
        for constructor in everything
    )
    table_entries = "".join(
        sub(
            '\t{ 0x@id~U, "@name~", TlJson_@cpp_name~ },\n',
            id="{:08x}".format(constructor.id),
            name=constructor.name,
            cpp_name=constructor.cpp_name,
        )
        for constructor in sorted(everything, key=lambda c: c.id)
    )

    write_functions_code = "".join(
        emit_write_constructor(constructor, types, bares) for constructor in everything
    )
    write_forward_declarations = "".join(
        sub(
            "bool TlJsonWrite_@cpp_name~(const QJsonObject &obj, mtpBuffer &to);\n",
            cpp_name=constructor.cpp_name,
        )
        for constructor in everything
    )
    write_table_entries = "".join(
        sub(
            '\t{ 0x@id~U, "@name~", TlJsonWrite_@cpp_name~ },\n',
            id="{:08x}".format(constructor.id),
            name=constructor.name,
            cpp_name=constructor.cpp_name,
        )
        for constructor in sorted(everything, key=lambda c: c.name)
    )

    header_path = pathlib.Path(output_path + "-tl_json.h").resolve()
    source_path = pathlib.Path(output_path + "-tl_json.cpp").resolve()
    header = _HEADER_TEMPLATE
    source = (
        _CPP_TEMPLATE.replace(
            "@include_name~", os.path.basename(output_path) + "-tl_json.h"
        )
        .replace("@bool_true~", "{:08x}".format(_BOOL_TRUE))
        .replace("@bool_false~", "{:08x}".format(_BOOL_FALSE))
        .replace("@vector~", "{:08x}".format(_VECTOR))
        .replace("@forward_declarations~", forward_declarations)
        .replace("@constructor_functions~", functions_code)
        .replace("@table_entries~", table_entries)
        .replace("@write_forward_declarations~", write_forward_declarations)
        .replace("@write_functions~", write_functions_code)
        .replace("@write_table_entries~", write_table_entries)
    )

    if not header_path.is_file() or header_path.read_text() != header:
        header_path.write_text(header)
    if not source_path.is_file() or source_path.read_text() != source:
        source_path.write_text(source)


def emit_constructor(constructor, types, bares):
    lines = [
        sub(
            "bool TlJson_@cpp_name~(QJsonObject &obj, const mtpPrime *&from, "
            "const mtpPrime *end) {\n",
            cpp_name=constructor.cpp_name,
        )
    ]
    for pname, ptype, cond in constructor.fields:
        lines += emit_field_code(pname, ptype, cond, types, bares)
    lines.append("\treturn true;\n}\n\n")
    return "".join(lines)


def emit_field_code(pname, ptype, cond, types, bares):
    if ptype == "#":
        return [
            sub(
                "\tif (!Has(from, end, 1)) {\n"
                "\t\treturn false;\n"
                "\t}\n"
                "\tconst auto v_@name~ = quint32(*from++);\n"
                '\tobj.insert(QLatin1String("@name~"), double(int32(v_@name~)));\n',
                name=pname,
            )
        ]

    body = emit_field_body(pname, ptype, types, bares)
    if cond is None:
        # Give every field body its own scope so that local declarations
        # of different fields never collide.
        return ["\t{\n", *body, "\t}\n"]
    return [
        sub("\tif ((v_@flags~ & (1U << @bit~)) != 0) {\n", flags=cond[0], bit=cond[1]),
        *body,
        "\t}\n",
    ]


def emit_field_body(pname, ptype, types, bares):
    kind, detail = classify(ptype, types, bares)
    if kind == "true":
        return [sub('\tobj.insert(QLatin1String("@name~"), true);\n', name=pname)]
    elif kind == "builtin":
        return [
            sub(
                '\tif (!Insert(obj, QLatin1String("@name~"), @reader~)) {\n'
                "\t\treturn false;\n"
                "\t}\n",
                name=pname,
                reader=_BUILTINS[detail],
            )
        ]
    elif kind == "bool":
        return [
            sub(
                '\tif (!Insert(obj, QLatin1String("@name~"), ReadBool(from, end))) {\n'
                "\t\treturn false;\n"
                "\t}\n",
                name=pname,
            )
        ]
    elif kind == "vector":
        return emit_vector(pname, detail, types, bares)
    elif kind == "bare":
        cons = types[detail][0]
        return [
            sub(
                "\t{\n"
                "\t\tauto child = QJsonObject();\n"
                '\t\tchild.insert(QStringLiteral("_"), QString::fromUtf8("@cons_name~"));\n'
                "\t\tif (!TlJson_@cpp_name~(child, from, end)) {\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                '\t\tobj.insert(QLatin1String("@name~"), child);\n'
                "\t}\n",
                name=pname,
                cons_name=cons.name,
                cpp_name=cons.cpp_name,
            )
        ]
    elif kind == "boxed":
        return [
            sub(
                '\tif (!Boxed(obj, QLatin1String("@name~"), from, end)) {\n'
                "\t\treturn false;\n"
                "\t}\n",
                name=pname,
            )
        ]
    raise ValueError("Bad field kind: " + kind)


def emit_vector(pname, element, types, bares):
    kind, detail = element
    lines = [
        "\tif (!Has(from, end, 2)) {\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\tif (quint32(*from++) != kVectorConstructor) {\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\tconst auto count = int32(quint32(*from++));\n"
        "\tif (count < 0) {\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\tauto list = QJsonArray();\n"
        "\tfor (auto i = 0; i != count; ++i) {\n"
    ]
    if kind == "builtin":
        lines.append(
            sub(
                "\t\tif (!Append(list, @reader~)) {\n\t\t\treturn false;\n\t\t}\n",
                reader=_BUILTINS[detail],
            )
        )
    elif kind == "bool":
        lines.append(
            "\t\tif (!Append(list, ReadBool(from, end))) {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
        )
    elif kind == "boxed":
        lines.append(
            "\t\tauto child = QJsonObject();\n"
            "\t\tif (!BoxedValue(child, from, end)) {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
            "\t\tlist.append(child);\n"
        )
    elif kind == "bare":
        cons = types[detail][0]
        lines.append(
            sub(
                "\t\tauto child = QJsonObject();\n"
                '\t\tchild.insert(QStringLiteral("_"), QString::fromUtf8("@cons_name~"));\n'
                "\t\tif (!TlJson_@cpp_name~(child, from, end)) {\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t\tlist.append(child);\n",
                cons_name=cons.name,
                cpp_name=cons.cpp_name,
            )
        )
    else:
        raise ValueError("Bad vector element kind: " + kind)
    lines.append("\t}\n")
    lines.append(sub('\tobj.insert(QLatin1String("@name~"), list);\n', name=pname))
    return lines


def emit_write_constructor(constructor, types, bares):
    lines = [
        sub(
            "bool TlJsonWrite_@cpp_name~(const QJsonObject &obj, mtpBuffer &to) {\n",
            cpp_name=constructor.cpp_name,
        )
    ]
    for pname, ptype, cond in constructor.fields:
        lines += emit_write_field_code(pname, ptype, cond, constructor, types, bares)
    lines.append("\treturn true;\n}\n\n")
    return "".join(lines)


def emit_write_field_code(pname, ptype, cond, constructor, types, bares):
    if ptype == "#":
        return emit_write_flags_code(pname, constructor)
    kind, detail = classify(ptype, types, bares)
    if kind == "true":
        # No bytes on the wire: the flag bit carries the presence, and it
        # was already counted at the flags field.
        return []
    body = emit_write_field_body(pname, ptype, types, bares)
    if cond is None:
        return ["\t{\n", *body, "\t}\n"]
    return [
        sub('\tif (obj.contains(QLatin1String("@name~"))) {\n', name=pname),
        *body,
        "\t}\n",
    ]


def emit_write_flags_code(pname, constructor):
    conditional = [
        (field_pname, cond[1])
        for field_pname, field_ptype, cond in constructor.fields
        if cond is not None and cond[0] == pname
    ]
    lines = [
        sub(
            '\tauto v_@name~ = quint32(0);\n'
            '\tif (const auto i = obj.constFind(QLatin1String("@name~"));'
            " i != obj.constEnd()) {\n"
            "\t\tif (!i->isDouble() || i->toDouble() != std::floor(i->toDouble())) {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
            "\t\tv_@name~ = quint32(int32(i->toDouble()));\n"
            "\t}\n",
            name=pname,
        )
    ]
    for field_pname, bit in conditional:
        lines.append(
            sub(
                '\tif (obj.contains(QLatin1String("@name~"))) {\n'
                "\t\tv_@flags~ |= (1U << @bit~);\n"
                "\t}\n",
                name=field_pname,
                flags=pname,
                bit=bit,
            )
        )
    lines.append(sub("\tto.push_back(mtpPrime(v_@name~));\n", name=pname))
    return lines


def emit_write_field_body(pname, ptype, types, bares):
    kind, detail = classify(ptype, types, bares)
    if kind == "builtin":
        writer = _BUILTIN_WRITERS[detail]
        if detail in ("int128", "int256"):
            call = sub(
                '@writer~(obj.value(QLatin1String("@name~")), @primes~, to)',
                writer=writer,
                name=pname,
                primes="4" if detail == "int128" else "8",
            )
        else:
            call = sub(
                '@writer~(obj.value(QLatin1String("@name~")), to)',
                writer=writer,
                name=pname,
            )
        return [
            sub("\tif (!@call~) {\n\t\treturn false;\n\t}\n", call=call)
        ]
    elif kind == "bool":
        return [
            sub(
                "\tif (!WriteBoolValue(obj.value(QLatin1String(\"@name~\")), to)) {\n"
                "\t\treturn false;\n"
                "\t}\n",
                name=pname,
            )
        ]
    elif kind == "vector":
        return emit_write_vector(pname, detail, types, bares)
    elif kind == "bare":
        cons = types[detail][0]
        return [
            sub(
                "\t{\n"
                '\t\tconst auto child = obj.value(QLatin1String("@name~")).toObject();\n'
                '\t\tconst auto name = child.value(QStringLiteral("_"));\n'
                '\t\tif (!name.isString() || name.toString() != QLatin1String("@cons_name~")) {\n'
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t\tif (!TlJsonWrite_@cpp_name~(child, to)) {\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t}\n",
                name=pname,
                cons_name=cons.name,
                cpp_name=cons.cpp_name,
            )
        ]
    elif kind == "boxed":
        return [
            sub(
                "\t{\n"
                '\t\tconst auto child = obj.value(QLatin1String("@name~"));\n'
                "\t\tif (!child.isObject() || !TlJsonEncodeBoxed(child.toObject(), to)) {\n"
                "\t\t\treturn false;\n"
                "\t\t}\n"
                "\t}\n",
                name=pname,
            )
        ]
    raise ValueError("Bad write field kind: " + kind)


def emit_write_vector(pname, element, types, bares):
    kind, detail = element
    lines = [
        sub(
            "\t{\n"
            '\t\tconst auto list = obj.value(QLatin1String("@name~"));\n'
            "\t\tif (!list.isArray()) {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
            "\t\tconst auto items = list.toArray();\n"
            "\t\tif (items.size() > 0x7FFFFFFF) {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
            "\t\tto.push_back(mtpPrime(kVectorConstructor));\n"
            "\t\tto.push_back(mtpPrime(items.size()));\n"
            "\t\tfor (const auto &item : items) {\n",
            name=pname,
        )
    ]
    if kind == "builtin":
        writer = _BUILTIN_WRITERS[detail]
        if detail in ("int128", "int256"):
            lines.append(
                sub(
                    "\t\t\tif (!@writer~(item, @primes~, to)) {\n"
                    "\t\t\t\treturn false;\n"
                    "\t\t\t}\n",
                    writer=writer,
                    primes="4" if detail == "int128" else "8",
                )
            )
        else:
            lines.append(
                sub(
                    "\t\t\tif (!@writer~(item, to)) {\n"
                    "\t\t\t\treturn false;\n"
                    "\t\t\t}\n",
                    writer=writer,
                )
            )
    elif kind == "bool":
        lines.append(
            "\t\t\tif (!WriteBoolValue(item, to)) {\n"
            "\t\t\t\treturn false;\n"
            "\t\t\t}\n"
        )
    elif kind == "boxed":
        lines.append(
            "\t\t\tif (!item.isObject() || !TlJsonEncodeBoxed(item.toObject(), to)) {\n"
            "\t\t\t\treturn false;\n"
            "\t\t\t}\n"
        )
    elif kind == "bare":
        cons = types[detail][0]
        lines.append(
            sub(
                "\t\t\tif (!item.isObject()) {\n"
                "\t\t\t\treturn false;\n"
                "\t\t\t}\n"
                "\t\t\tconst auto child = item.toObject();\n"
                '\t\t\tconst auto name = child.value(QStringLiteral("_"));\n'
                '\t\t\tif (!name.isString() || name.toString() != QLatin1String("@cons_name~")) {\n'
                "\t\t\t\treturn false;\n"
                "\t\t\t}\n"
                "\t\t\tif (!TlJsonWrite_@cpp_name~(child, to)) {\n"
                "\t\t\t\treturn false;\n"
                "\t\t\t}\n",
                cons_name=cons.name,
                cpp_name=cons.cpp_name,
            )
        )
    else:
        raise ValueError("Bad write vector element kind: " + kind)
    lines.append("\t\t}\n\t}\n")
    return lines
