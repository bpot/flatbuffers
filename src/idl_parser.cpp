/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

namespace flatbuffers {

const char *const kTypeNames[] = {
  #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) IDLTYPE,
    FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
  #undef FLATBUFFERS_TD
  nullptr
};

const char kTypeSizes[] = {
  #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) sizeof(CTYPE),
    FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
  #undef FLATBUFFERS_TD
};

static void Error(const std::string &msg) {
  throw msg;
}

// Ensure that integer values we parse fit inside the declared integer type.
static void CheckBitsFit(int64_t val, size_t bits) {
  auto mask = (1ll << bits) - 1;  // Bits we allow to be used.
  if (bits < 64 &&
      (val & ~mask) != 0 &&  // Positive or unsigned.
      (val |  mask) != -1)   // Negative.
    Error("constant does not fit in a " + NumToString(bits) + "-bit field");
}

// atot: templated version of atoi/atof: convert a string to an instance of T.
template<typename T> inline T atot(const char *s) {
  auto val = StringToInt(s);
  CheckBitsFit(val, sizeof(T) * 8);
  return (T)val;
}
template<> inline bool atot<bool>(const char *s) {
  return 0 != atoi(s);
}
template<> inline float atot<float>(const char *s) {
  return static_cast<float>(strtod(s, nullptr));
}
template<> inline double atot<double>(const char *s) {
  return strtod(s, nullptr);
}

template<> inline Offset<void> atot<Offset<void>>(const char *s) {
  return Offset<void>(atoi(s));
}

// Declare tokens we'll use. Single character tokens are represented by their
// ascii character code (e.g. '{'), others above 256.
#define FLATBUFFERS_GEN_TOKENS(TD) \
  TD(Eof, 256, "end of file") \
  TD(StringConstant, 257, "string constant") \
  TD(IntegerConstant, 258, "integer constant") \
  TD(FloatConstant, 259, "float constant") \
  TD(Identifier, 260, "identifier") \
  TD(Table, 261, "table") \
  TD(Struct, 262, "struct") \
  TD(Enum, 263, "enum") \
  TD(Union, 264, "union") \
  TD(NameSpace, 265, "namespace") \
  TD(RootType, 266, "root_type")
#ifdef __GNUC__
__extension__  // Stop GCC complaining about trailing comma with -Wpendantic.
#endif
enum {
  #define FLATBUFFERS_TOKEN(NAME, VALUE, STRING) kToken ## NAME = VALUE,
    FLATBUFFERS_GEN_TOKENS(FLATBUFFERS_TOKEN)
  #undef FLATBUFFERS_TOKEN
  #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) kToken ## ENUM,
    FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
  #undef FLATBUFFERS_TD
};

static std::string TokenToString(int t) {
  static const char *tokens[] = {
    #define FLATBUFFERS_TOKEN(NAME, VALUE, STRING) STRING,
      FLATBUFFERS_GEN_TOKENS(FLATBUFFERS_TOKEN)
    #undef FLATBUFFERS_TOKEN
    #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) IDLTYPE,
      FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
    #undef FLATBUFFERS_TD
  };
  if (t < 256) {  // A single ascii char token.
    std::string s;
    s.append(1, t);
    return s;
  } else {       // Other tokens.
    return tokens[t - 256];
  }
}

void Parser::Next() {
  doc_comment_.clear();
  bool seen_newline = false;
  for (;;) {
    char c = *cursor_++;
    token_ = c;
    switch (c) {
      case '\0': cursor_--; token_ = kTokenEof; return;
      case ' ': case '\r': case '\t': break;
      case '\n': line_++; seen_newline = true; break;
      case '{': case '}': case '(': case ')': case '[': case ']': return;
      case ',': case ':': case ';': case '=': return;
      case '.':
        if(!isdigit(*cursor_)) return;
        Error("floating point constant can\'t start with \".\"");
        break;
      case '\"':
        attribute_ = "";
        while (*cursor_ != '\"') {
          if (*cursor_ < ' ' && *cursor_ >= 0)
            Error("illegal character in string constant");
          if (*cursor_ == '\\') {
            cursor_++;
            switch (*cursor_) {
              case 'n':  attribute_ += '\n'; cursor_++; break;
              case 't':  attribute_ += '\t'; cursor_++; break;
              case 'r':  attribute_ += '\r'; cursor_++; break;
              case '\"': attribute_ += '\"'; cursor_++; break;
              case '\\': attribute_ += '\\'; cursor_++; break;
              default: Error("unknown escape code in string constant"); break;
            }
          } else { // printable chars + UTF-8 bytes
            attribute_ += *cursor_++;
          }
        }
        cursor_++;
        token_ = kTokenStringConstant;
        return;
      case '/':
        if (*cursor_ == '/') {
          const char *start = ++cursor_;
          while (*cursor_ && *cursor_ != '\n') cursor_++;
          if (*start == '/') {  // documentation comment
            if (!seen_newline)
              Error("a documentation comment should be on a line on its own");
            // todo: do we want to support multiline comments instead?
            doc_comment_ += std::string(start + 1, cursor_);
          }
          break;
        }
        // fall thru
      default:
        if (isalpha(static_cast<unsigned char>(c))) {
          // Collect all chars of an identifier:
          const char *start = cursor_ - 1;
          while (isalnum(static_cast<unsigned char>(*cursor_)) ||
                 *cursor_ == '_')
            cursor_++;
          attribute_.clear();
          attribute_.append(start, cursor_);
          // First, see if it is a type keyword from the table of types:
          #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) \
            if (attribute_ == IDLTYPE) { \
              token_ = kToken ## ENUM; \
              return; \
            }
            FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
          #undef FLATBUFFERS_TD
          // If it's a boolean constant keyword, turn those into integers,
          // which simplifies our logic downstream.
          if (attribute_ == "true" || attribute_ == "false") {
            attribute_ = NumToString(attribute_ == "true");
            token_ = kTokenIntegerConstant;
            return;
          }
          // Check for declaration keywords:
          if (attribute_ == "table")     { token_ = kTokenTable;     return; }
          if (attribute_ == "struct")    { token_ = kTokenStruct;    return; }
          if (attribute_ == "enum")      { token_ = kTokenEnum;      return; }
          if (attribute_ == "union")     { token_ = kTokenUnion;     return; }
          if (attribute_ == "namespace") { token_ = kTokenNameSpace; return; }
          if (attribute_ == "root_type") { token_ = kTokenRootType;  return; }
          // If not, it is a user-defined identifier:
          token_ = kTokenIdentifier;
          return;
        } else if (isdigit(static_cast<unsigned char>(c)) || c == '-') {
          const char *start = cursor_ - 1;
          while (isdigit(static_cast<unsigned char>(*cursor_))) cursor_++;
          if (*cursor_ == '.') {
            cursor_++;
            while (isdigit(static_cast<unsigned char>(*cursor_))) cursor_++;
            // See if this float has a scientific notation suffix. Both JSON
            // and C++ (through strtod() we use) have the same format:
            if (*cursor_ == 'e' || *cursor_ == 'E') {
              cursor_++;
              if (*cursor_ == '+' || *cursor_ == '-') cursor_++;
              while (isdigit(static_cast<unsigned char>(*cursor_))) cursor_++;
            }
            token_ = kTokenFloatConstant;
          } else {
            token_ = kTokenIntegerConstant;
          }
          attribute_.clear();
          attribute_.append(start, cursor_);
          return;
        }
        std::string ch;
        ch = c;
        if (c < ' ' || c > '~') ch = "code: " + NumToString(c);
        Error("illegal character: " + ch);
        break;
    }
  }
}

// Check if a given token is next, if so, consume it as well.
bool Parser::IsNext(int t) {
  bool isnext = t == token_;
  if (isnext) Next();
  return isnext;
}

// Expect a given token to be next, consume it, or error if not present.
void Parser::Expect(int t) {
  if (t != token_) {
    Error("expecting: " + TokenToString(t) + " instead got: " +
          TokenToString(token_));
  }
  Next();
}

// Parse any IDL type.
void Parser::ParseType(Type &type) {
  if (token_ >= kTokenBOOL && token_ <= kTokenSTRING) {
    type.base_type = static_cast<BaseType>(token_ - kTokenNONE);
  } else {
    if (token_ == kTokenIdentifier) {
      auto enum_def = enums_.Lookup(attribute_);
      if (enum_def) {
        type = enum_def->underlying_type;
        if (enum_def->is_union) type.base_type = BASE_TYPE_UNION;
      } else {
        type.base_type = BASE_TYPE_STRUCT;
        type.struct_def = LookupCreateStruct(attribute_);
      }
    } else if (token_ == '[') {
      Next();
      Type subtype;
      ParseType(subtype);
      if (subtype.base_type == BASE_TYPE_VECTOR) {
        // We could support this, but it will complicate things, and it's
        // easier to work around with a struct around the inner vector.
        Error("nested vector types not supported (wrap in table first).");
      }
      if (subtype.base_type == BASE_TYPE_UNION) {
        // We could support this if we stored a struct of 2 elements per
        // union element.
        Error("vector of union types not supported (wrap in table first).");
      }
      type = Type(BASE_TYPE_VECTOR, subtype.struct_def, subtype.enum_def);
      type.element = subtype.base_type;
      Expect(']');
      return;
    } else {
      Error("illegal type syntax");
    }
  }
  Next();
}

FieldDef &Parser::AddField(StructDef &struct_def,
                           const std::string &name,
                           const Type &type) {
  auto &field = *new FieldDef();
  field.value.offset =
    FieldIndexToOffset(static_cast<voffset_t>(struct_def.fields.vec.size()));
  field.name = name;
  field.value.type = type;
  if (struct_def.fixed) {  // statically compute the field offset
    auto size = InlineSize(type);
    auto alignment = InlineAlignment(type);
    // structs_ need to have a predictable format, so we need to align to
    // the largest scalar
    struct_def.minalign = std::max(struct_def.minalign, alignment);
    struct_def.PadLastField(alignment);
    field.value.offset = static_cast<voffset_t>(struct_def.bytesize);
    struct_def.bytesize += size;
  }
  if (struct_def.fields.Add(name, &field))
    Error("field already exists: " + name);
  return field;
}

void Parser::ParseField(StructDef &struct_def) {
  std::string name = attribute_;
  std::string dc = doc_comment_;
  Expect(kTokenIdentifier);
  Expect(':');
  Type type;
  ParseType(type);

  if (struct_def.fixed && !IsScalar(type.base_type) && !IsStruct(type))
    Error("structs_ may contain only scalar or struct fields");

  FieldDef *typefield = nullptr;
  if (type.base_type == BASE_TYPE_UNION) {
    // For union fields, add a second auto-generated field to hold the type,
    // with _type appended as the name.
    typefield = &AddField(struct_def, name + "_type",
                          type.enum_def->underlying_type);
  }

  auto &field = AddField(struct_def, name, type);

  if (token_ == '=') {
    Next();
    ParseSingleValue(field.value);
  }

  field.doc_comment = dc;
  ParseMetaData(field);
  field.deprecated = field.attributes.Lookup("deprecated") != nullptr;
  if (field.deprecated && struct_def.fixed)
    Error("can't deprecate fields in a struct");
  auto nested = field.attributes.Lookup("nested_flatbuffer");
  if (nested) {
    if (nested->type.base_type != BASE_TYPE_STRING)
      Error("nested_flatbuffer attribute must be a string (the root type)");
    if (field.value.type.base_type != BASE_TYPE_VECTOR ||
        field.value.type.element != BASE_TYPE_UCHAR)
      Error("nested_flatbuffer attribute may only apply to a vector of ubyte");
    // This will cause an error if the root type of the nested flatbuffer
    // wasn't defined elsewhere.
    LookupCreateStruct(nested->constant);
  }

  if (typefield) {
    // If this field is a union, and it has a manually assigned id,
    // the automatically added type field should have an id as well (of N - 1).
    auto attr = field.attributes.Lookup("id");
    if (attr) {
      auto id = atoi(attr->constant.c_str());
      auto val = new Value();
      val->type = attr->type;
      val->constant = NumToString(id - 1);
      typefield->attributes.Add("id", val);
    }
  }

  Expect(';');
}

void Parser::ParseAnyValue(Value &val, FieldDef *field) {
  switch (val.type.base_type) {
    case BASE_TYPE_UNION: {
      assert(field);
      if (!field_stack_.size() ||
          field_stack_.back().second->value.type.base_type != BASE_TYPE_UTYPE)
        Error("missing type field before this union value: " + field->name);
      auto enum_idx = atot<unsigned char>(
                                    field_stack_.back().first.constant.c_str());
      auto enum_val = val.type.enum_def->ReverseLookup(enum_idx);
      if (!enum_val) Error("illegal type id for: " + field->name);
      val.constant = NumToString(ParseTable(*enum_val->struct_def));
      break;
    }
    case BASE_TYPE_STRUCT:
      val.constant = NumToString(ParseTable(*val.type.struct_def));
      break;
    case BASE_TYPE_STRING: {
      auto s = attribute_;
      Expect(kTokenStringConstant);
      val.constant = NumToString(builder_.CreateString(s).o);
      break;
    }
    case BASE_TYPE_VECTOR: {
      Expect('[');
      val.constant = NumToString(ParseVector(val.type.VectorType()));
      break;
    }
    default:
      ParseSingleValue(val);
      break;
  }
}

void Parser::SerializeStruct(const StructDef &struct_def, const Value &val) {
  auto off = atot<uoffset_t>(val.constant.c_str());
  assert(struct_stack_.size() - off == struct_def.bytesize);
  builder_.Align(struct_def.minalign);
  builder_.PushBytes(&struct_stack_[off], struct_def.bytesize);
  struct_stack_.resize(struct_stack_.size() - struct_def.bytesize);
  builder_.AddStructOffset(val.offset, builder_.GetSize());
}

uoffset_t Parser::ParseTable(const StructDef &struct_def) {
  Expect('{');
  size_t fieldn = 0;
  for (;;) {
    std::string name = attribute_;
    if (!IsNext(kTokenStringConstant)) Expect(kTokenIdentifier);
    auto field = struct_def.fields.Lookup(name);
    if (!field) Error("unknown field: " + name);
    if (struct_def.fixed && (fieldn >= struct_def.fields.vec.size()
                            || struct_def.fields.vec[fieldn] != field)) {
       Error("struct field appearing out of order: " + name);
    }
    Expect(':');
    Value val = field->value;
    ParseAnyValue(val, field);
    field_stack_.push_back(std::make_pair(val, field));
    fieldn++;
    if (IsNext('}')) break;
    Expect(',');
  }
  if (struct_def.fixed && fieldn != struct_def.fields.vec.size())
    Error("incomplete struct initialization: " + struct_def.name);
  auto start = struct_def.fixed
                 ? builder_.StartStruct(struct_def.minalign)
                 : builder_.StartTable();

  for (size_t size = struct_def.sortbysize ? sizeof(largest_scalar_t) : 1;
       size;
       size /= 2) {
    // Go through elements in reverse, since we're building the data backwards.
    for (auto it = field_stack_.rbegin();
             it != field_stack_.rbegin() + fieldn; ++it) {
      auto &value = it->first;
      auto field = it->second;
      if (!struct_def.sortbysize || size == SizeOf(value.type.base_type)) {
        switch (value.type.base_type) {
          #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) \
            case BASE_TYPE_ ## ENUM: \
              builder_.Pad(field->padding); \
              builder_.AddElement(value.offset, \
                             atot<CTYPE>(       value.constant.c_str()), \
                             atot<CTYPE>(field->value.constant.c_str())); \
              break;
            FLATBUFFERS_GEN_TYPES_SCALAR(FLATBUFFERS_TD);
          #undef FLATBUFFERS_TD
          #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) \
            case BASE_TYPE_ ## ENUM: \
              builder_.Pad(field->padding); \
              if (IsStruct(field->value.type)) { \
                SerializeStruct(*field->value.type.struct_def, value); \
              } else { \
                builder_.AddOffset(value.offset, \
                  atot<CTYPE>(value.constant.c_str())); \
              } \
              break;
            FLATBUFFERS_GEN_TYPES_POINTER(FLATBUFFERS_TD);
          #undef FLATBUFFERS_TD
        }
      }
    }
  }
  for (size_t i = 0; i < fieldn; i++) field_stack_.pop_back();

  if (struct_def.fixed) {
    builder_.ClearOffsets();
    builder_.EndStruct();
    // Temporarily store this struct in a side buffer, since this data has to
    // be stored in-line later in the parent object.
    auto off = struct_stack_.size();
    struct_stack_.insert(struct_stack_.end(),
                         builder_.GetBufferPointer(),
                         builder_.GetBufferPointer() + struct_def.bytesize);
    builder_.PopBytes(struct_def.bytesize);
    return static_cast<uoffset_t>(off);
  } else {
    return builder_.EndTable(
      start,
      static_cast<voffset_t>(struct_def.fields.vec.size()));
  }
}

uoffset_t Parser::ParseVector(const Type &type) {
  int count = 0;
  if (token_ != ']') for (;;) {
    Value val;
    val.type = type;
    ParseAnyValue(val, NULL);
    field_stack_.push_back(std::make_pair(val, nullptr));
    count++;
    if (token_ == ']') break;
    Expect(',');
  }
  Next();

  builder_.StartVector(count * InlineSize(type), InlineAlignment((type)));
  for (int i = 0; i < count; i++) {
    // start at the back, since we're building the data backwards.
    auto &val = field_stack_.back().first;
    switch (val.type.base_type) {
      #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE) \
        case BASE_TYPE_ ## ENUM: \
          if (IsStruct(val.type)) SerializeStruct(*val.type.struct_def, val); \
          else builder_.PushElement(atot<CTYPE>(val.constant.c_str())); \
          break;
        FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
      #undef FLATBUFFERS_TD
    }
    field_stack_.pop_back();
  }

  builder_.ClearOffsets();
  return builder_.EndVector(count);
}

void Parser::ParseMetaData(Definition &def) {
  if (IsNext('(')) {
    for (;;) {
      auto name = attribute_;
      Expect(kTokenIdentifier);
      auto e = new Value();
      def.attributes.Add(name, e);
      if (IsNext(':')) {
        ParseSingleValue(*e);
      }
      if (IsNext(')')) break;
      Expect(',');
    }
  }
}

bool Parser::TryTypedValue(int dtoken,
                           bool check,
                           Value &e,
                           BaseType req) {
  bool match = dtoken == token_;
  if (match) {
    e.constant = attribute_;
    if (!check) {
      if (e.type.base_type == BASE_TYPE_NONE) {
        e.type.base_type = req;
      } else {
        Error(std::string("type mismatch: expecting: ") +
              kTypeNames[e.type.base_type] +
              ", found: " +
              kTypeNames[req]);
      }
    }
    Next();
  }
  return match;
}

int64_t Parser::ParseIntegerFromString(Type &type) {
  int64_t result = 0;
  // Parse one or more enum identifiers, separated by spaces.
  const char *next = attribute_.c_str();
  do {
    const char *divider = strchr(next, ' ');
    std::string word;
    if (divider) {
      word = std::string(next, divider);
      next = divider + strspn(divider, " ");
    } else {
      word = next;
      next += word.length();
    }
    if (type.enum_def) {  // The field has an enum type
      auto enum_val = type.enum_def->vals.Lookup(word);
      if (!enum_val)
        Error("unknown enum value: " + word +
              ", for enum: " + type.enum_def->name);
      result |= enum_val->value;
    } else {  // No enum type, probably integral field.
      if (!IsInteger(type.base_type))
        Error("not a valid value for this field: " + word);
      // TODO: could check if its a valid number constant here.
      const char *dot = strchr(word.c_str(), '.');
      if (!dot) Error("enum values need to be qualified by an enum type");
      std::string enum_def_str(word.c_str(), dot);
      std::string enum_val_str(dot + 1, word.c_str() + word.length());
      auto enum_def = enums_.Lookup(enum_def_str);
      if (!enum_def) Error("unknown enum: " + enum_def_str);
      auto enum_val = enum_def->vals.Lookup(enum_val_str);
      if (!enum_val) Error("unknown enum value: " + enum_val_str);
      result |= enum_val->value;
    }
  } while(*next);
  return result;
}

void Parser::ParseSingleValue(Value &e) {
  // First check if this could be a string/identifier enum value:
  if (e.type.base_type != BASE_TYPE_STRING &&
      e.type.base_type != BASE_TYPE_NONE &&
      (token_ == kTokenIdentifier || token_ == kTokenStringConstant)) {
      e.constant = NumToString(ParseIntegerFromString(e.type));
      Next();
  } else if (TryTypedValue(kTokenIntegerConstant,
                    IsScalar(e.type.base_type),
                    e,
                    BASE_TYPE_INT) ||
      TryTypedValue(kTokenFloatConstant,
                    IsFloat(e.type.base_type),
                    e,
                    BASE_TYPE_FLOAT) ||
      TryTypedValue(kTokenStringConstant,
                    e.type.base_type == BASE_TYPE_STRING,
                    e,
                    BASE_TYPE_STRING)) {
  } else {
    Error("cannot parse value starting with: " + TokenToString(token_));
  }
}

StructDef *Parser::LookupCreateStruct(const std::string &name) {
  auto struct_def = structs_.Lookup(name);
  if (!struct_def) {
    // Rather than failing, we create a "pre declared" StructDef, due to
    // circular references, and check for errors at the end of parsing.
    struct_def = new StructDef();
    structs_.Add(name, struct_def);
    struct_def->name = name;
    struct_def->predecl = true;
  }
  return struct_def;
}

void Parser::ParseEnum(bool is_union) {
  std::string dc = doc_comment_;
  Next();
  std::string name = attribute_;
  Expect(kTokenIdentifier);
  auto &enum_def = *new EnumDef();
  enum_def.name = name;
  enum_def.doc_comment = dc;
  enum_def.is_union = is_union;
  if (enums_.Add(name, &enum_def)) Error("enum already exists: " + name);
  if (is_union) {
    enum_def.underlying_type.base_type = BASE_TYPE_UTYPE;
    enum_def.underlying_type.enum_def = &enum_def;
  } else {
    // Give specialized error message, since this type spec used to
    // be optional in the first FlatBuffers release.
    if (!IsNext(':')) Error("must specify the underlying integer type for this"
                            " enum (e.g. \': short\', which was the default).");
    // Specify the integer type underlying this enum.
    ParseType(enum_def.underlying_type);
    if (!IsInteger(enum_def.underlying_type.base_type))
      Error("underlying enum type must be integral");
    // Make this type refer back to the enum it was derived from.
    enum_def.underlying_type.enum_def = &enum_def;
  }
  ParseMetaData(enum_def);
  Expect('{');
  if (is_union) enum_def.vals.Add("NONE", new EnumVal("NONE", 0));
  do {
    std::string name = attribute_;
    std::string dc = doc_comment_;
    Expect(kTokenIdentifier);
    auto prevsize = enum_def.vals.vec.size();
    auto value = enum_def.vals.vec.size()
      ? enum_def.vals.vec.back()->value + 1
      : 0;
    auto &ev = *new EnumVal(name, value);
    if (enum_def.vals.Add(name, &ev))
      Error("enum value already exists: " + name);
    ev.doc_comment = dc;
    if (is_union) {
      ev.struct_def = LookupCreateStruct(name);
    }
    if (IsNext('=')) {
      ev.value = atoi(attribute_.c_str());
      Expect(kTokenIntegerConstant);
      if (prevsize && enum_def.vals.vec[prevsize - 1]->value >= ev.value)
        Error("enum values must be specified in ascending order");
    }
  } while (IsNext(','));
  Expect('}');
  if (enum_def.attributes.Lookup("bit_flags")) {
    for (auto it = enum_def.vals.vec.begin(); it != enum_def.vals.vec.end();
         ++it) {
      if (static_cast<size_t>((*it)->value) >=
           SizeOf(enum_def.underlying_type.base_type) * 8)
        Error("bit flag out of range of underlying integral type");
      (*it)->value = 1LL << (*it)->value;
    }
  }
}

void Parser::ParseDecl() {
  std::string dc = doc_comment_;
  bool fixed = IsNext(kTokenStruct);
  if (!fixed) Expect(kTokenTable);
  std::string name = attribute_;
  Expect(kTokenIdentifier);
  auto &struct_def = *LookupCreateStruct(name);
  if (!struct_def.predecl) Error("datatype already exists: " + name);
  struct_def.predecl = false;
  struct_def.name = name;
  struct_def.doc_comment = dc;
  struct_def.fixed = fixed;
  // Move this struct to the back of the vector just in case it was predeclared,
  // to preserve declartion order.
  remove(structs_.vec.begin(), structs_.vec.end(), &struct_def);
  structs_.vec.back() = &struct_def;
  ParseMetaData(struct_def);
  struct_def.sortbysize =
    struct_def.attributes.Lookup("original_order") == nullptr && !fixed;
  Expect('{');
  while (token_ != '}') ParseField(struct_def);
  auto force_align = struct_def.attributes.Lookup("force_align");
  if (fixed && force_align) {
    auto align = static_cast<size_t>(atoi(force_align->constant.c_str()));
    if (force_align->type.base_type != BASE_TYPE_INT ||
        align < struct_def.minalign ||
        align > 256 ||
        align & (align - 1))
      Error("force_align must be a power of two integer ranging from the"
            "struct\'s natural alignment to 256");
    struct_def.minalign = align;
  }
  struct_def.PadLastField(struct_def.minalign);
  // Check if this is a table that has manual id assignments
  auto &fields = struct_def.fields.vec;
  if (!struct_def.fixed && fields.size()) {
    size_t num_id_fields = 0;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
      if ((*it)->attributes.Lookup("id")) num_id_fields++;
    }
    // If any fields have ids..
    if (num_id_fields) {
      // Then all fields must have them.
      if (num_id_fields != fields.size())
        Error("either all fields or no fields must have an 'id' attribute");
      // Simply sort by id, then the fields are the same as if no ids had
      // been specified.
      std::sort(fields.begin(), fields.end(),
        [](const FieldDef *a, const FieldDef *b) -> bool {
          auto a_id = atoi(a->attributes.Lookup("id")->constant.c_str());
          auto b_id = atoi(b->attributes.Lookup("id")->constant.c_str());
          return a_id < b_id;
      });
      // Verify we have a contiguous set, and reassign vtable offsets.
      for (int i = 0; i < static_cast<int>(fields.size()); i++) {
        if (i != atoi(fields[i]->attributes.Lookup("id")->constant.c_str()))
          Error("field id\'s must be consecutive from 0, id " +
                NumToString(i) + " missing or set twice");
        fields[i]->value.offset = FieldIndexToOffset(static_cast<voffset_t>(i));
      }
    }
  }
  Expect('}');
}

bool Parser::SetRootType(const char *name) {
  root_struct_def = structs_.Lookup(name);
  return root_struct_def != nullptr;
}

bool Parser::Parse(const char *source) {
  source_ = cursor_ = source;
  line_ = 1;
  error_.clear();
  builder_.Clear();
  try {
    Next();
    while (token_ != kTokenEof) {
      if (token_ == kTokenNameSpace) {
        Next();
        name_space_.clear();
        for (;;) {
          name_space_.push_back(attribute_);
          Expect(kTokenIdentifier);
          if (!IsNext('.')) break;
        }
        Expect(';');
      } else if (token_ == '{') {
        if (!root_struct_def) Error("no root type set to parse json with");
        if (builder_.GetSize()) {
          Error("cannot have more than one json object in a file");
        }
        builder_.Finish(Offset<Table>(ParseTable(*root_struct_def)));
      } else if (token_ == kTokenEnum) {
        ParseEnum(false);
      } else if (token_ == kTokenUnion) {
        ParseEnum(true);
      } else if (token_ == kTokenRootType) {
        Next();
        auto root_type = attribute_;
        Expect(kTokenIdentifier);
        Expect(';');
        if (!SetRootType(root_type.c_str()))
          Error("unknown root type: " + root_type);
        if (root_struct_def->fixed)
          Error("root type must be a table");
      } else {
        ParseDecl();
      }
    }
    for (auto it = structs_.vec.begin(); it != structs_.vec.end(); ++it) {
      if ((*it)->predecl)
        Error("type referenced but not defined: " + (*it)->name);
    }
    for (auto it = enums_.vec.begin(); it != enums_.vec.end(); ++it) {
      auto &enum_def = **it;
      if (enum_def.is_union) {
        for (auto it = enum_def.vals.vec.begin();
             it != enum_def.vals.vec.end();
             ++it) {
          auto &val = **it;
          if (val.struct_def && val.struct_def->fixed)
            Error("only tables can be union elements: " + val.name);
        }
      }
    }
  } catch (const std::string &msg) {
    error_ = "line " + NumToString(line_) + ": " + msg;
    return false;
  }
  assert(!struct_stack_.size());
  return true;
}

}  // namespace flatbuffers
