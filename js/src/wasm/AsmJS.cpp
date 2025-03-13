/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * Copyright 2014 Mozilla Foundation
 * Copyright 2022-2024 Moonchild Productions
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

#include "wasm/AsmJS.h"

#include "mozilla/Attributes.h"
#include "mozilla/Compression.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include "jsmath.h"
#include "jsprf.h"
#include "jsstr.h"
#include "jsutil.h"

#include "jswrapper.h"

#include "frontend/Parser.h"
#include "gc/Policy.h"
#include "js/MemoryMetrics.h"
#include "js/SourceBufferHolder.h"
#include "vm/SelfHosting.h"
#include "vm/StringBuffer.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmValidate.h"

#include "jsobjinlines.h"

#include "frontend/ParseNode-inl.h"
#include "vm/ArrayBufferObject-inl.h"

using namespace js;
using namespace js::frontend;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CeilingLog2;
using mozilla::Compression::LZ4;
using mozilla::HashGeneric;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;
using mozilla::IsPositiveZero;
using mozilla::IsPowerOfTwo;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::PodCopy;
using mozilla::PodEqual;
using mozilla::PodZero;
using mozilla::PositiveInfinity;
using JS::AsmJSOption;
using JS::GenericNaN;
using JS::SourceBufferHolder;

/*****************************************************************************/

// The asm.js valid heap lengths are precisely the WASM valid heap lengths for ARM
// greater or equal to MinHeapLength
static const size_t MinHeapLength = PageSize;

static uint32_t
RoundUpToNextValidAsmJSHeapLength(uint32_t length)
{
    if (length <= MinHeapLength)
        return MinHeapLength;

    return wasm::RoundUpToNextValidARMImmediate(length);
}


/*****************************************************************************/
// asm.js module object

// The asm.js spec recognizes this set of builtin Math functions.
enum AsmJSMathBuiltinFunction
{
    AsmJSMathBuiltin_sin, AsmJSMathBuiltin_cos, AsmJSMathBuiltin_tan,
    AsmJSMathBuiltin_asin, AsmJSMathBuiltin_acos, AsmJSMathBuiltin_atan,
    AsmJSMathBuiltin_ceil, AsmJSMathBuiltin_floor, AsmJSMathBuiltin_exp,
    AsmJSMathBuiltin_log, AsmJSMathBuiltin_pow, AsmJSMathBuiltin_sqrt,
    AsmJSMathBuiltin_abs, AsmJSMathBuiltin_atan2, AsmJSMathBuiltin_imul,
    AsmJSMathBuiltin_fround, AsmJSMathBuiltin_min, AsmJSMathBuiltin_max,
    AsmJSMathBuiltin_clz32
};

// The asm.js spec will recognize this set of builtin Atomics functions.
enum AsmJSAtomicsBuiltinFunction
{
    AsmJSAtomicsBuiltin_compareExchange,
    AsmJSAtomicsBuiltin_exchange,
    AsmJSAtomicsBuiltin_load,
    AsmJSAtomicsBuiltin_store,
    AsmJSAtomicsBuiltin_add,
    AsmJSAtomicsBuiltin_sub,
    AsmJSAtomicsBuiltin_and,
    AsmJSAtomicsBuiltin_or,
    AsmJSAtomicsBuiltin_xor,
    AsmJSAtomicsBuiltin_isLockFree
};


// An AsmJSGlobal represents a JS global variable in the asm.js module function.
class AsmJSGlobal
{
  public:
    enum Which { Variable, FFI, ArrayView, ArrayViewCtor, MathBuiltinFunction,
                 AtomicsBuiltinFunction, Constant };
    enum VarInitKind { InitConstant, InitImport };
    enum ConstantKind { GlobalConstant, MathConstant };

  private:
    struct CacheablePod {
        Which which_;
        union V {
            struct {
                VarInitKind initKind_;
                union U {
                    ValType importType_;
                    Val val_;
                    U() {}
                } u;
            } var;
            uint32_t ffiIndex_;
            Scalar::Type viewType_;
            AsmJSMathBuiltinFunction mathBuiltinFunc_;
            AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
            struct {
                ConstantKind kind_;
                double value_;
            } constant;
            V() {}
        } u;
    } pod;
    CacheableChars field_;

    friend class ModuleValidator;

  public:
    AsmJSGlobal() = default;
    AsmJSGlobal(Which which, UniqueChars field) {
        mozilla::PodZero(&pod);  // zero padding for Valgrind
        pod.which_ = which;
        field_ = Move(field);
    }
    const char* field() const {
        return field_.get();
    }
    Which which() const {
        return pod.which_;
    }
    VarInitKind varInitKind() const {
        MOZ_ASSERT(pod.which_ == Variable);
        return pod.u.var.initKind_;
    }
    Val varInitVal() const {
        MOZ_ASSERT(pod.which_ == Variable);
        MOZ_ASSERT(pod.u.var.initKind_ == InitConstant);
        return pod.u.var.u.val_;
    }
    ValType varInitImportType() const {
        MOZ_ASSERT(pod.which_ == Variable);
        MOZ_ASSERT(pod.u.var.initKind_ == InitImport);
        return pod.u.var.u.importType_;
    }
    uint32_t ffiIndex() const {
        MOZ_ASSERT(pod.which_ == FFI);
        return pod.u.ffiIndex_;
    }
    // When a view is created from an imported constructor:
    //   var I32 = stdlib.Int32Array;
    //   var i32 = new I32(buffer);
    // the second import has nothing to validate and thus has a null field.
    Scalar::Type viewType() const {
        MOZ_ASSERT(pod.which_ == ArrayView || pod.which_ == ArrayViewCtor);
        return pod.u.viewType_;
    }
    AsmJSMathBuiltinFunction mathBuiltinFunction() const {
        MOZ_ASSERT(pod.which_ == MathBuiltinFunction);
        return pod.u.mathBuiltinFunc_;
    }
    AsmJSAtomicsBuiltinFunction atomicsBuiltinFunction() const {
        MOZ_ASSERT(pod.which_ == AtomicsBuiltinFunction);
        return pod.u.atomicsBuiltinFunc_;
    }
    ConstantKind constantKind() const {
        MOZ_ASSERT(pod.which_ == Constant);
        return pod.u.constant.kind_;
    }
    double constantValue() const {
        MOZ_ASSERT(pod.which_ == Constant);
        return pod.u.constant.value_;
    }

    WASM_DECLARE_SERIALIZABLE(AsmJSGlobal);
};

typedef Vector<AsmJSGlobal, 0, SystemAllocPolicy> AsmJSGlobalVector;

// An AsmJSImport is slightly different than an asm.js FFI function: a single
// asm.js FFI function can be called with many different signatures. When
// compiled to wasm, each unique FFI function paired with signature generates a
// wasm import.
class AsmJSImport
{
    uint32_t ffiIndex_;
  public:
    AsmJSImport() = default;
    explicit AsmJSImport(uint32_t ffiIndex) : ffiIndex_(ffiIndex) {}
    uint32_t ffiIndex() const { return ffiIndex_; }
};

typedef Vector<AsmJSImport, 0, SystemAllocPolicy> AsmJSImportVector;

// An AsmJSExport logically extends Export with the extra information needed for
// an asm.js exported function, viz., the offsets in module's source chars in
// case the function is toString()ed.
class AsmJSExport
{
    uint32_t funcIndex_ = 0;

    // All fields are treated as cacheable POD:
    uint32_t startOffsetInModule_ = 0;  // Store module-start-relative offsets
    uint32_t endOffsetInModule_ = 0;    // so preserved by serialization.

  public:
    AsmJSExport() = default;
    AsmJSExport(uint32_t funcIndex, uint32_t startOffsetInModule, uint32_t endOffsetInModule)
      : funcIndex_(funcIndex),
        startOffsetInModule_(startOffsetInModule),
        endOffsetInModule_(endOffsetInModule)
    {}
    uint32_t funcIndex() const {
        return funcIndex_;
    }
    uint32_t startOffsetInModule() const {
        return startOffsetInModule_;
    }
    uint32_t endOffsetInModule() const {
        return endOffsetInModule_;
    }
};

typedef Vector<AsmJSExport, 0, SystemAllocPolicy> AsmJSExportVector;

enum class CacheResult
{
    Hit,
    Miss
};

// Holds the immutable guts of an AsmJSModule.
//
// AsmJSMetadata is built incrementally by ModuleValidator and then shared
// immutably between AsmJSModules.

struct AsmJSMetadataCacheablePod
{
    uint32_t                numFFIs = 0;
    uint32_t                srcLength = 0;
    uint32_t                srcLengthWithRightBrace = 0;

    AsmJSMetadataCacheablePod() = default;
};

struct js::AsmJSMetadata : Metadata, AsmJSMetadataCacheablePod
{
    AsmJSGlobalVector       asmJSGlobals;
    AsmJSImportVector       asmJSImports;
    AsmJSExportVector       asmJSExports;
    CacheableCharsVector    asmJSFuncNames;
    CacheableChars          globalArgumentName;
    CacheableChars          importArgumentName;
    CacheableChars          bufferArgumentName;

    CacheResult             cacheResult;

    // These values are not serialized since they are relative to the
    // containing script which can be different between serialization and
    // deserialization contexts. Thus, they must be set explicitly using the
    // ambient Parser/ScriptSource after deserialization.
    //
    // srcStart refers to the offset in the ScriptSource to the beginning of
    // the asm.js module function. If the function has been created with the
    // Function constructor, this will be the first character in the function
    // source. Otherwise, it will be the opening parenthesis of the arguments
    // list.
    uint32_t                toStringStart;
    uint32_t                srcStart;
    uint32_t                srcBodyStart;
    bool                    strict;
    ScriptSourceHolder      scriptSource;

    uint32_t srcEndBeforeCurly() const {
        return srcStart + srcLength;
    }
    uint32_t srcEndAfterCurly() const {
        return srcStart + srcLengthWithRightBrace;
    }

    AsmJSMetadata()
      : Metadata(ModuleKind::AsmJS),
        cacheResult(CacheResult::Miss),
        srcStart(0),
        srcBodyStart(0),
        strict(false)
    {}
    ~AsmJSMetadata() override {}

    const AsmJSExport& lookupAsmJSExport(uint32_t funcIndex) const {
        // The AsmJSExportVector isn't stored in sorted order so do a linear
        // search. This is for the super-cold and already-expensive toString()
        // path and the number of exports is generally small.
        for (const AsmJSExport& exp : asmJSExports) {
            if (exp.funcIndex() == funcIndex)
                return exp;
        }
        MOZ_CRASH("missing asm.js func export");
    }

    bool mutedErrors() const override {
        return scriptSource.get()->mutedErrors();
    }
    const char16_t* displayURL() const override {
        return scriptSource.get()->hasDisplayURL() ? scriptSource.get()->displayURL() : nullptr;
    }
    ScriptSource* maybeScriptSource() const override {
        return scriptSource.get();
    }
    bool getFuncName(const Bytes* maybeBytecode, uint32_t funcIndex, UTF8Bytes* name) const override {
        // asm.js doesn't allow exporting imports or putting imports in tables
        MOZ_ASSERT(funcIndex >= AsmJSFirstDefFuncIndex);
        const char* p = asmJSFuncNames[funcIndex - AsmJSFirstDefFuncIndex].get();
        return name->append(p, strlen(p));
    }

    AsmJSMetadataCacheablePod& pod() { return *this; }
    const AsmJSMetadataCacheablePod& pod() const { return *this; }

    WASM_DECLARE_SERIALIZABLE_OVERRIDE(AsmJSMetadata)
};

typedef RefPtr<AsmJSMetadata> MutableAsmJSMetadata;

/*****************************************************************************/
// ParseNode utilities

static inline ParseNode*
NextNode(ParseNode* pn)
{
    return pn->pn_next;
}

static inline ParseNode*
UnaryKid(ParseNode* pn)
{
    return pn->as<UnaryNode>().kid();
}

static inline ParseNode*
BinaryRight(ParseNode* pn)
{
    return pn->as<BinaryNode>().right();
}

static inline ParseNode*
BinaryLeft(ParseNode* pn)
{
    return pn->as<BinaryNode>().left();
}

static inline ParseNode*
ReturnExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_RETURN));
    return UnaryKid(pn);
}

static inline ParseNode*
TernaryKid1(ParseNode* pn)
{
    return pn->as<TernaryNode>().kid1();
}

static inline ParseNode*
TernaryKid2(ParseNode* pn)
{
    return pn->as<TernaryNode>().kid2();
}

static inline ParseNode*
TernaryKid3(ParseNode* pn)
{
    return pn->as<TernaryNode>().kid3();
}

static inline ParseNode*
ListHead(ParseNode* pn)
{
    return pn->as<ListNode>().head();
}

static inline unsigned
ListLength(ParseNode* pn)
{
    return pn->as<ListNode>().count();
}

static inline ParseNode*
CallCallee(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    return BinaryLeft(pn);
}

static inline unsigned
CallArgListLength(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    return ListLength(BinaryRight(pn));
}

static inline ParseNode*
CallArgList(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    return ListHead(BinaryRight(pn));
}

static inline ParseNode*
VarListHead(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_VAR) || pn->isKind(PNK_CONST));
    return ListHead(pn);
}

static inline bool
IsDefaultCase(ParseNode* pn)
{
    return pn->as<CaseClause>().isDefault();
}

static inline ParseNode*
CaseExpr(ParseNode* pn)
{
    return pn->as<CaseClause>().caseExpression();
}

static inline ParseNode*
CaseBody(ParseNode* pn)
{
    return pn->as<CaseClause>().statementList();
}

static inline ParseNode*
BinaryOpLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isBinaryOperation());
    MOZ_ASSERT(pn->as<ListNode>().count() == 2);
    return ListHead(pn);
}

static inline ParseNode*
BinaryOpRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isBinaryOperation());
    MOZ_ASSERT(pn->as<ListNode>().count() == 2);
    return NextNode(ListHead(pn));
}

static inline ParseNode*
BitwiseLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
BitwiseRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline ParseNode*
MultiplyLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_STAR));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
MultiplyRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_STAR));
    return BinaryOpRight(pn);
}

static inline ParseNode*
AddSubLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ADD) || pn->isKind(PNK_SUB));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
AddSubRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ADD) || pn->isKind(PNK_SUB));
    return BinaryOpRight(pn);
}

static inline ParseNode*
DivOrModLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DIV) || pn->isKind(PNK_MOD));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
DivOrModRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DIV) || pn->isKind(PNK_MOD));
    return BinaryOpRight(pn);
}

static inline ParseNode*
ComparisonLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
ComparisonRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline bool
IsExpressionStatement(ParseNode* pn)
{
    return pn->isKind(PNK_SEMI);
}

static inline ParseNode*
ExpressionStatementExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_SEMI));
    return UnaryKid(pn);
}

static inline PropertyName*
LoopControlMaybeLabel(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_BREAK) || pn->isKind(PNK_CONTINUE));
    return pn->as<LoopControlStatement>().label();
}

static inline PropertyName*
LabeledStatementLabel(ParseNode* pn)
{
    return pn->as<LabeledStatement>().label();
}

static inline ParseNode*
LabeledStatementStatement(ParseNode* pn)
{
    return pn->as<LabeledStatement>().statement();
}

static double
NumberNodeValue(ParseNode* pn)
{
    return pn->as<NumericLiteral>().value();
}

static bool
NumberNodeHasFrac(ParseNode* pn)
{
    return pn->as<NumericLiteral>().decimalPoint() == HasDecimal;
}

static ParseNode*
DotBase(ParseNode* pn)
{
    return &pn->as<PropertyAccess>().expression();
}

static PropertyName*
DotMember(ParseNode* pn)
{
    return &pn->as<PropertyAccess>().name();
}

static ParseNode*
ElemBase(ParseNode* pn)
{
    return &pn->as<PropertyByValueBase>().expression();
}

static ParseNode*
ElemIndex(ParseNode* pn)
{
    return &pn->as<PropertyByValueBase>().key();
}

static inline JSFunction*
FunctionObject(FunctionNode* funNode)
{
    MOZ_ASSERT(funNode->isKind(PNK_FUNCTION));
    return funNode->funbox()->function();
}

static inline PropertyName*
FunctionName(FunctionNode* funNode)
{
    if (JSAtom* name = FunctionObject(funNode)->explicitName())
        return name->asPropertyName();
    return nullptr;
}

static inline ParseNode*
FunctionStatementList(FunctionNode* funNode)
{
    MOZ_ASSERT(funNode->body()->isKind(PNK_PARAMSBODY));
    LexicalScopeNode* last = &funNode->body()->as<ListNode>().last()->as<LexicalScopeNode>();
    MOZ_ASSERT(last->isEmptyScope());
    ParseNode* body = last->scopeBody();
    MOZ_ASSERT(body->isKind(PNK_STATEMENTLIST));
    return body;
}

static inline bool
IsNormalObjectField(ExclusiveContext* cx, ParseNode* pn)
{
    return pn->isKind(PNK_COLON) &&
           pn->getOp() == JSOP_INITPROP &&
           BinaryLeft(pn)->isKind(PNK_OBJECT_PROPERTY_NAME);
}

static inline PropertyName*
ObjectNormalFieldName(ExclusiveContext* cx, ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(cx, pn));
    MOZ_ASSERT(BinaryLeft(pn)->isKind(PNK_OBJECT_PROPERTY_NAME));
    return BinaryLeft(pn)->as<NameNode>().atom()->asPropertyName();
}

static inline ParseNode*
ObjectNormalFieldInitializer(ExclusiveContext* cx, ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(cx, pn));
    return BinaryRight(pn);
}

static inline ParseNode*
MaybeInitializer(ParseNode* pn)
{
    return pn->as<NameNode>().initializer();
}

static inline bool
IsUseOfName(ParseNode* pn, PropertyName* name)
{
    return pn->isKind(PNK_NAME) && pn->name() == name;
}

static inline bool
IsIgnoredDirectiveName(ExclusiveContext* cx, JSAtom* atom)
{
    return atom != cx->names().useStrict;
}

static inline bool
IsIgnoredDirective(ExclusiveContext* cx, ParseNode* pn)
{
    return pn->isKind(PNK_SEMI) &&
           UnaryKid(pn) &&
           UnaryKid(pn)->isKind(PNK_STRING) &&
           IsIgnoredDirectiveName(cx, UnaryKid(pn)->as<NameNode>().atom());
}

static inline bool
IsEmptyStatement(ParseNode* pn)
{
    return pn->isKind(PNK_SEMI) && !UnaryKid(pn);
}

static inline ParseNode*
SkipEmptyStatements(ParseNode* pn)
{
    while (pn && IsEmptyStatement(pn))
        pn = pn->pn_next;
    return pn;
}

static inline ParseNode*
NextNonEmptyStatement(ParseNode* pn)
{
    return SkipEmptyStatements(pn->pn_next);
}

static bool
GetToken(AsmJSParser& parser, TokenKind* tkp)
{
    TokenStream& ts = parser.tokenStream;
    TokenKind tk;
    while (true) {
        if (!ts.getToken(&tk, TokenStream::Operand))
            return false;
        if (tk != TOK_SEMI)
            break;
    }
    *tkp = tk;
    return true;
}

static bool
PeekToken(AsmJSParser& parser, TokenKind* tkp)
{
    TokenStream& ts = parser.tokenStream;
    TokenKind tk;
    while (true) {
        if (!ts.peekToken(&tk, TokenStream::Operand))
            return false;
        if (tk != TOK_SEMI)
            break;
        ts.consumeKnownToken(TOK_SEMI, TokenStream::Operand);
    }
    *tkp = tk;
    return true;
}

static bool
ParseVarOrConstStatement(AsmJSParser& parser, ParseNode** var)
{
    TokenKind tk;
    if (!PeekToken(parser, &tk))
        return false;
    if (tk != TOK_VAR && tk != TOK_CONST) {
        *var = nullptr;
        return true;
    }

    *var = parser.statementListItem(YieldIsName);
    if (!*var)
        return false;

    MOZ_ASSERT((*var)->isKind(PNK_VAR) || (*var)->isKind(PNK_CONST));
    return true;
}

/*****************************************************************************/

// Represents the type and value of an asm.js numeric literal.
//
// A literal is a double iff the literal contains a decimal point (even if the
// fractional part is 0). Otherwise, integers may be classified:
//  fixnum: [0, 2^31)
//  negative int: [-2^31, 0)
//  big unsigned: [2^31, 2^32)
//  out of range: otherwise
// Lastly, a literal may be a float literal which is any double or integer
// literal coerced with Math.fround.
//
class NumLit
{
  public:
    enum Which {
        Fixnum,
        NegativeInt,
        BigUnsigned,
        Double,
        Float,
        OutOfRangeInt = -1
    };

  private:
    Which which_;
    union {
        JS::UninitializedValue scalar_;
    } u;

  public:
    NumLit() = default;

    NumLit(Which w, const Value& v) : which_(w) {
        u.scalar_ = v;
    }

    Which which() const {
        return which_;
    }

    int32_t toInt32() const {
        MOZ_ASSERT(which_ == Fixnum || which_ == NegativeInt || which_ == BigUnsigned);
        return u.scalar_.asValueRef().toInt32();
    }

    uint32_t toUint32() const {
        return (uint32_t)toInt32();
    }

    double toDouble() const {
        MOZ_ASSERT(which_ == Double);
        return u.scalar_.asValueRef().toDouble();
    }

    float toFloat() const {
        MOZ_ASSERT(which_ == Float);
        return float(u.scalar_.asValueRef().toDouble());
    }

    Value scalarValue() const {
        MOZ_ASSERT(which_ != OutOfRangeInt);
        return u.scalar_.asValueRef();
    }

    bool valid() const {
        return which_ != OutOfRangeInt;
    }

    bool isZeroBits() const {
        MOZ_ASSERT(valid());
        switch (which()) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return toInt32() == 0;
          case NumLit::Double:
            return IsPositiveZero(toDouble());
          case NumLit::Float:
            return IsPositiveZero(toFloat());
          case NumLit::OutOfRangeInt:
            MOZ_CRASH("can't be here because of valid() check above");
        }
        return false;
    }

    Val value() const {
        switch (which_) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return Val(toUint32());
          case NumLit::Float:
            return Val(toFloat());
          case NumLit::Double:
            return Val(toDouble());
          case NumLit::OutOfRangeInt:;
        }
        MOZ_CRASH("bad literal");
    }
};

// Represents the type of a general asm.js expression.
//
// A canonical subset of types representing the coercion targets: Int, Float,
// Double.
//
// Void is also part of the canonical subset which then maps to wasm::ExprType.

class Type
{
  public:
    enum Which {
        Fixnum = NumLit::Fixnum,
        Signed = NumLit::NegativeInt,
        Unsigned = NumLit::BigUnsigned,
        DoubleLit = NumLit::Double,
        Float = NumLit::Float,
        Double,
        MaybeDouble,
        MaybeFloat,
        Floatish,
        Int,
        Intish,
        Void
    };

  private:
    Which which_;

  public:
    Type() = default;
    MOZ_IMPLICIT Type(Which w) : which_(w) {}

    // Map an already canonicalized Type to the return type of a function call.
    static Type ret(Type t) {
        MOZ_ASSERT(t.isCanonical());
        // The 32-bit external type is Signed, not Int.
        return t.isInt() ? Signed: t;
    }

    static Type lit(const NumLit& lit) {
        MOZ_ASSERT(lit.valid());
        Which which = Type::Which(lit.which());
        MOZ_ASSERT(which >= Fixnum && which <= Float);
        Type t;
        t.which_ = which;
        return t;
    }

    // Map |t| to one of the canonical vartype representations of a
    // wasm::ExprType.
    static Type canonicalize(Type t) {
        switch(t.which()) {
          case Fixnum:
          case Signed:
          case Unsigned:
          case Int:
            return Int;

          case Float:
            return Float;

          case DoubleLit:
          case Double:
            return Double;

          case Void:
            return Void;

          case MaybeDouble:
          case MaybeFloat:
          case Floatish:
          case Intish:
            // These types need some kind of coercion, they can't be mapped
            // to an ExprType.
            break;
        }
        MOZ_CRASH("Invalid vartype");
    }

    Which which() const { return which_; }

    bool operator==(Type rhs) const { return which_ == rhs.which_; }
    bool operator!=(Type rhs) const { return which_ != rhs.which_; }

    bool operator<=(Type rhs) const {
        switch (rhs.which_) {
          case Signed:      return isSigned();
          case Unsigned:    return isUnsigned();
          case DoubleLit:   return isDoubleLit();
          case Double:      return isDouble();
          case Float:       return isFloat();
          case MaybeDouble: return isMaybeDouble();
          case MaybeFloat:  return isMaybeFloat();
          case Floatish:    return isFloatish();
          case Int:         return isInt();
          case Intish:      return isIntish();
          case Fixnum:      return isFixnum();
          case Void:        return isVoid();
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected rhs type");
    }

    bool isFixnum() const {
        return which_ == Fixnum;
    }

    bool isSigned() const {
        return which_ == Signed || which_ == Fixnum;
    }

    bool isUnsigned() const {
        return which_ == Unsigned || which_ == Fixnum;
    }

    bool isInt() const {
        return isSigned() || isUnsigned() || which_ == Int;
    }

    bool isIntish() const {
        return isInt() || which_ == Intish;
    }

    bool isDoubleLit() const {
        return which_ == DoubleLit;
    }

    bool isDouble() const {
        return isDoubleLit() || which_ == Double;
    }

    bool isMaybeDouble() const {
        return isDouble() || which_ == MaybeDouble;
    }

    bool isFloat() const {
        return which_ == Float;
    }

    bool isMaybeFloat() const {
        return isFloat() || which_ == MaybeFloat;
    }

    bool isFloatish() const {
        return isMaybeFloat() || which_ == Floatish;
    }

    bool isVoid() const {
        return which_ == Void;
    }

    bool isExtern() const {
        return isDouble() || isSigned();
    }

    // Check if this is one of the valid types for a function argument.
    bool isArgType() const {
        return isInt() || isFloat() || isDouble();
    }

    // Check if this is one of the valid types for a function return value.
    bool isReturnType() const {
        return isSigned() || isFloat() || isDouble() || isVoid();
    }

    // Check if this is one of the valid types for a global variable.
    bool isGlobalVarType() const {
        return isArgType();
    }

    // Check if this is one of the canonical vartype representations of a
    // wasm::ExprType. See Type::canonicalize().
    bool isCanonical() const {
        switch (which()) {
          case Int:
          case Float:
          case Double:
          case Void:
            return true;
          default:
            return false;
        }
    }

    // Check if this is a canonical representation of a wasm::ValType.
    bool isCanonicalValType() const {
        return !isVoid() && isCanonical();
    }

    // Convert this canonical type to a wasm::ExprType.
    ExprType canonicalToExprType() const {
        switch (which()) {
          case Int:       return ExprType::I32;
          case Float:     return ExprType::F32;
          case Double:    return ExprType::F64;
          case Void:      return ExprType::Void;
          default:        MOZ_CRASH("Need canonical type");
        }
    }

    // Convert this canonical type to a wasm::ValType.
    ValType canonicalToValType() const {
        return NonVoidToValType(canonicalToExprType());
    }

    // Convert this type to a wasm::ExprType for use in a wasm
    // block signature. This works for all types, including non-canonical
    // ones. Consequently, the type isn't valid for subsequent asm.js
    // validation; it's only valid for use in producing wasm.
    ExprType toWasmBlockSignatureType() const {
        switch (which()) {
          case Fixnum:
          case Signed:
          case Unsigned:
          case Int:
          case Intish:
            return ExprType::I32;

          case Float:
          case MaybeFloat:
          case Floatish:
            return ExprType::F32;

          case DoubleLit:
          case Double:
          case MaybeDouble:
            return ExprType::F64;

          case Void:
            return ExprType::Void;
        }
        MOZ_CRASH("Invalid Type");
    }

    const char* toChars() const {
        switch (which_) {
          case Double:      return "double";
          case DoubleLit:   return "doublelit";
          case MaybeDouble: return "double?";
          case Float:       return "float";
          case Floatish:    return "floatish";
          case MaybeFloat:  return "float?";
          case Fixnum:      return "fixnum";
          case Int:         return "int";
          case Signed:      return "signed";
          case Unsigned:    return "unsigned";
          case Intish:      return "intish";
          case Void:        return "void";
        }
        MOZ_CRASH("Invalid Type");
    }
};

static const unsigned VALIDATION_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;

// The ModuleValidator encapsulates the entire validation of an asm.js module.
// Its lifetime goes from the validation of the top components of an asm.js
// module (all the globals), the emission of bytecode for all the functions in
// the module and the validation of function's pointer tables. It also finishes
// the compilation of all the module's stubs.
//
// Rooting note: ModuleValidator is a stack class that contains unrooted
// PropertyName (JSAtom) pointers.  This is safe because it cannot be
// constructed without a TokenStream reference.  TokenStream is itself a stack
// class that cannot be constructed without an AutoKeepAtoms being live on the
// stack, which prevents collection of atoms.
//
// ModuleValidator is marked as rooted in the rooting analysis.  Don't add
// non-JSAtom pointers, or this will break!
class MOZ_STACK_CLASS ModuleValidator
{
  public:
    class Func
    {
        PropertyName* name_;
        uint32_t firstUse_;
        uint32_t index_;
        uint32_t srcBegin_;
        uint32_t srcEnd_;
        bool defined_;

      public:
        Func(PropertyName* name, uint32_t firstUse, uint32_t index)
          : name_(name), firstUse_(firstUse), index_(index),
            srcBegin_(0), srcEnd_(0), defined_(false)
        {}

        PropertyName* name() const { return name_; }
        uint32_t firstUse() const { return firstUse_; }
        bool defined() const { return defined_; }
        uint32_t index() const { return index_; }

        void define(ParseNode* fn) {
            MOZ_ASSERT(!defined_);
            defined_ = true;
            srcBegin_ = fn->pn_pos.begin;
            srcEnd_ = fn->pn_pos.end;
        }

        uint32_t srcBegin() const { MOZ_ASSERT(defined_); return srcBegin_; }
        uint32_t srcEnd() const { MOZ_ASSERT(defined_); return srcEnd_; }
    };

    typedef Vector<const Func*> ConstFuncVector;
    typedef Vector<Func*> FuncVector;

    class FuncPtrTable
    {
        uint32_t sigIndex_;
        PropertyName* name_;
        uint32_t firstUse_;
        uint32_t mask_;
        bool defined_;

        FuncPtrTable(FuncPtrTable&& rhs) = delete;

      public:
        FuncPtrTable(uint32_t sigIndex, PropertyName* name, uint32_t firstUse, uint32_t mask)
          : sigIndex_(sigIndex), name_(name), firstUse_(firstUse), mask_(mask), defined_(false)
        {}

        uint32_t sigIndex() const { return sigIndex_; }
        PropertyName* name() const { return name_; }
        uint32_t firstUse() const { return firstUse_; }
        unsigned mask() const { return mask_; }
        bool defined() const { return defined_; }
        void define() { MOZ_ASSERT(!defined_); defined_ = true; }
    };

    typedef Vector<FuncPtrTable*> FuncPtrTableVector;

    class Global
    {
      public:
        enum Which {
            Variable,
            ConstantLiteral,
            ConstantImport,
            Function,
            FuncPtrTable,
            FFI,
            ArrayView,
            ArrayViewCtor,
            MathBuiltinFunction,
            AtomicsBuiltinFunction
        };

      private:
        Which which_;
        union {
            struct {
                Type::Which type_;
                unsigned index_;
                NumLit literalValue_;
            } varOrConst;
            uint32_t funcIndex_;
            uint32_t funcPtrTableIndex_;
            uint32_t ffiIndex_;
            struct {
                Scalar::Type viewType_;
            } viewInfo;
            AsmJSMathBuiltinFunction mathBuiltinFunc_;
            AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
        } u;

        friend class ModuleValidator;
        friend class js::LifoAlloc;

        explicit Global(Which which) : which_(which) {}
        
      public:
      
        Which which() const {
            return which_;
        }
        Type varOrConstType() const {
            MOZ_ASSERT(which_ == Variable || which_ == ConstantLiteral || which_ == ConstantImport);
            return u.varOrConst.type_;
        }
        unsigned varOrConstIndex() const {
            MOZ_ASSERT(which_ == Variable || which_ == ConstantImport);
            return u.varOrConst.index_;
        }
        bool isConst() const {
            return which_ == ConstantLiteral || which_ == ConstantImport;
        }
        NumLit constLiteralValue() const {
            MOZ_ASSERT(which_ == ConstantLiteral);
            return u.varOrConst.literalValue_;
        }
        uint32_t funcIndex() const {
            MOZ_ASSERT(which_ == Function);
            return u.funcIndex_;
        }
        uint32_t funcPtrTableIndex() const {
            MOZ_ASSERT(which_ == FuncPtrTable);
            return u.funcPtrTableIndex_;
        }
        unsigned ffiIndex() const {
            MOZ_ASSERT(which_ == FFI);
            return u.ffiIndex_;
        }
        bool isAnyArrayView() const {
            return which_ == ArrayView || which_ == ArrayViewCtor;
        }
        Scalar::Type viewType() const {
            MOZ_ASSERT(isAnyArrayView());
            return u.viewInfo.viewType_;
        }
        bool isMathFunction() const {
            return which_ == MathBuiltinFunction;
        }
        AsmJSMathBuiltinFunction mathBuiltinFunction() const {
            MOZ_ASSERT(which_ == MathBuiltinFunction);
            return u.mathBuiltinFunc_;
        }
        bool isAtomicsFunction() const {
            return which_ == AtomicsBuiltinFunction;
        }
        AsmJSAtomicsBuiltinFunction atomicsBuiltinFunction() const {
            MOZ_ASSERT(which_ == AtomicsBuiltinFunction);
            return u.atomicsBuiltinFunc_;
        }
    };

    struct MathBuiltin
    {
        enum Kind { Function, Constant };
        Kind kind;

        union {
            double cst;
            AsmJSMathBuiltinFunction func;
        } u;

        MathBuiltin() : kind(Kind(-1)) {}
        explicit MathBuiltin(double cst) : kind(Constant) {
            u.cst = cst;
        }
        explicit MathBuiltin(AsmJSMathBuiltinFunction func) : kind(Function) {
            u.func = func;
        }
    };

    struct ArrayView
    {
        ArrayView(PropertyName* name, Scalar::Type type)
          : name(name), type(type)
        {}

        PropertyName* name;
        Scalar::Type type;
    };

  private:
    class NamedSig
    {
        PropertyName* name_;
        const SigWithId* sig_;

      public:
        NamedSig(PropertyName* name, const SigWithId& sig)
          : name_(name), sig_(&sig)
        {}
        PropertyName* name() const {
            return name_;
        }
        const Sig& sig() const {
            return *sig_;
        }

        // Implement HashPolicy:
        struct Lookup {
            PropertyName* name;
            const Sig& sig;
            Lookup(PropertyName* name, const Sig& sig) : name(name), sig(sig) {}
        };
        static HashNumber hash(Lookup l) {
            return HashGeneric(l.name, l.sig.hash());
        }
        static bool match(NamedSig lhs, Lookup rhs) {
            return lhs.name_ == rhs.name && *lhs.sig_ == rhs.sig;
        }
    };
    typedef HashMap<NamedSig, uint32_t, NamedSig> ImportMap;
    typedef HashMap<const SigWithId*, uint32_t, SigHashPolicy> SigMap;
    typedef HashMap<PropertyName*, Global*> GlobalMap;
    typedef HashMap<PropertyName*, MathBuiltin> MathNameMap;
    typedef HashMap<PropertyName*, AsmJSAtomicsBuiltinFunction> AtomicsNameMap;
    typedef Vector<ArrayView> ArrayViewVector;

    ExclusiveContext*     cx_;
    AsmJSParser&          parser_;
    FunctionNode*         moduleFunctionNode_;
    PropertyName*         moduleFunctionName_;
    PropertyName*         globalArgumentName_;
    PropertyName*         importArgumentName_;
    PropertyName*         bufferArgumentName_;
    MathNameMap           standardLibraryMathNames_;
    AtomicsNameMap        standardLibraryAtomicsNames_;
    RootedFunction        dummyFunction_;

    // Validation-internal state:
    LifoAlloc             validationLifo_;
    FuncVector            functions_;
    FuncPtrTableVector    funcPtrTables_;
    GlobalMap             globalMap_;
    SigMap                sigMap_;
    ImportMap             importMap_;
    ArrayViewVector       arrayViews_;
    bool                  atomicsPresent_;

    // State used to build the AsmJSModule in finish():
    ModuleGenerator       mg_;
    MutableAsmJSMetadata  asmJSMetadata_;

    // Error reporting:
    UniqueChars           errorString_;
    uint32_t              errorOffset_;
    bool                  errorOverRecursed_;

    // Helpers:
    bool addStandardLibraryMathName(const char* name, AsmJSMathBuiltinFunction func) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        MathBuiltin builtin(func);
        return standardLibraryMathNames_.putNew(atom->asPropertyName(), builtin);
    }
    bool addStandardLibraryMathName(const char* name, double cst) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        MathBuiltin builtin(cst);
        return standardLibraryMathNames_.putNew(atom->asPropertyName(), builtin);
    }
    bool addStandardLibraryAtomicsName(const char* name, AsmJSAtomicsBuiltinFunction func) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        return standardLibraryAtomicsNames_.putNew(atom->asPropertyName(), func);
    }
    bool newSig(Sig&& sig, uint32_t* sigIndex) {
        *sigIndex = 0;
        if (mg_.numSigs() >= AsmJSMaxTypes)
            return failCurrentOffset("too many signatures");

        *sigIndex = mg_.numSigs();
        mg_.initSig(*sigIndex, Move(sig));
        return true;
    }
    bool declareSig(Sig&& sig, uint32_t* sigIndex) {
        SigMap::AddPtr p = sigMap_.lookupForAdd(sig);
        if (p) {
            *sigIndex = p->value();
            MOZ_ASSERT(mg_.sig(*sigIndex) == sig);
            return true;
        }

        return newSig(Move(sig), sigIndex) &&
               sigMap_.add(p, &mg_.sig(*sigIndex), *sigIndex);
    }

  public:
    ModuleValidator(ExclusiveContext* cx, AsmJSParser& parser, FunctionNode* moduleFunctionNode)
      : cx_(cx),
        parser_(parser),
        moduleFunctionNode_(moduleFunctionNode),
        moduleFunctionName_(FunctionName(moduleFunctionNode)),
        globalArgumentName_(nullptr),
        importArgumentName_(nullptr),
        bufferArgumentName_(nullptr),
        standardLibraryMathNames_(cx),
        standardLibraryAtomicsNames_(cx),
        dummyFunction_(cx),
        validationLifo_(VALIDATION_LIFO_DEFAULT_CHUNK_SIZE),
        functions_(cx),
        funcPtrTables_(cx),
        globalMap_(cx),
        sigMap_(cx),
        importMap_(cx),
        arrayViews_(cx),
        atomicsPresent_(false),
        mg_(nullptr),
        errorString_(nullptr),
        errorOffset_(UINT32_MAX),
        errorOverRecursed_(false)
    {}

    ~ModuleValidator() {
        if (errorString_) {
            MOZ_ASSERT(errorOffset_ != UINT32_MAX);
            tokenStream().reportAsmJSError(errorOffset_,
                                           JSMSG_USE_ASM_TYPE_FAIL,
                                           errorString_.get());
        }
        if (errorOverRecursed_)
            ReportOverRecursed(cx_);
    }

    bool init() {
        asmJSMetadata_ = cx_->new_<AsmJSMetadata>();
        if (!asmJSMetadata_)
            return false;

        asmJSMetadata_->toStringStart = moduleFunctionNode_->funbox()->toStringStart;
        asmJSMetadata_->srcStart = moduleFunctionNode_->body()->pn_pos.begin;
        asmJSMetadata_->srcBodyStart = parser_.tokenStream.currentToken().pos.end;
        asmJSMetadata_->strict = parser_.pc->sc()->strict() &&
                                 !parser_.pc->sc()->hasExplicitUseStrict();
        asmJSMetadata_->scriptSource.reset(parser_.ss);

        if (!globalMap_.init() || !sigMap_.init() || !importMap_.init())
            return false;

        if (!standardLibraryMathNames_.init() ||
            !addStandardLibraryMathName("sin", AsmJSMathBuiltin_sin) ||
            !addStandardLibraryMathName("cos", AsmJSMathBuiltin_cos) ||
            !addStandardLibraryMathName("tan", AsmJSMathBuiltin_tan) ||
            !addStandardLibraryMathName("asin", AsmJSMathBuiltin_asin) ||
            !addStandardLibraryMathName("acos", AsmJSMathBuiltin_acos) ||
            !addStandardLibraryMathName("atan", AsmJSMathBuiltin_atan) ||
            !addStandardLibraryMathName("ceil", AsmJSMathBuiltin_ceil) ||
            !addStandardLibraryMathName("floor", AsmJSMathBuiltin_floor) ||
            !addStandardLibraryMathName("exp", AsmJSMathBuiltin_exp) ||
            !addStandardLibraryMathName("log", AsmJSMathBuiltin_log) ||
            !addStandardLibraryMathName("pow", AsmJSMathBuiltin_pow) ||
            !addStandardLibraryMathName("sqrt", AsmJSMathBuiltin_sqrt) ||
            !addStandardLibraryMathName("abs", AsmJSMathBuiltin_abs) ||
            !addStandardLibraryMathName("atan2", AsmJSMathBuiltin_atan2) ||
            !addStandardLibraryMathName("imul", AsmJSMathBuiltin_imul) ||
            !addStandardLibraryMathName("clz32", AsmJSMathBuiltin_clz32) ||
            !addStandardLibraryMathName("fround", AsmJSMathBuiltin_fround) ||
            !addStandardLibraryMathName("min", AsmJSMathBuiltin_min) ||
            !addStandardLibraryMathName("max", AsmJSMathBuiltin_max) ||
            !addStandardLibraryMathName("E", M_E) ||
            !addStandardLibraryMathName("LN10", M_LN10) ||
            !addStandardLibraryMathName("LN2", M_LN2) ||
            !addStandardLibraryMathName("LOG2E", M_LOG2E) ||
            !addStandardLibraryMathName("LOG10E", M_LOG10E) ||
            !addStandardLibraryMathName("PI", M_PI) ||
            !addStandardLibraryMathName("SQRT1_2", M_SQRT1_2) ||
            !addStandardLibraryMathName("SQRT2", M_SQRT2))
        {
            return false;
        }

        if (!standardLibraryAtomicsNames_.init() ||
            !addStandardLibraryAtomicsName("compareExchange", AsmJSAtomicsBuiltin_compareExchange) ||
            !addStandardLibraryAtomicsName("exchange", AsmJSAtomicsBuiltin_exchange) ||
            !addStandardLibraryAtomicsName("load", AsmJSAtomicsBuiltin_load) ||
            !addStandardLibraryAtomicsName("store", AsmJSAtomicsBuiltin_store) ||
            !addStandardLibraryAtomicsName("add", AsmJSAtomicsBuiltin_add) ||
            !addStandardLibraryAtomicsName("sub", AsmJSAtomicsBuiltin_sub) ||
            !addStandardLibraryAtomicsName("and", AsmJSAtomicsBuiltin_and) ||
            !addStandardLibraryAtomicsName("or", AsmJSAtomicsBuiltin_or) ||
            !addStandardLibraryAtomicsName("xor", AsmJSAtomicsBuiltin_xor) ||
            !addStandardLibraryAtomicsName("isLockFree", AsmJSAtomicsBuiltin_isLockFree))
        {
            return false;
        }

        // This flows into FunctionBox, so must be tenured.
        dummyFunction_ = NewScriptedFunction(cx_, 0, JSFunction::INTERPRETED, nullptr,
                                             /* proto = */ nullptr, gc::AllocKind::FUNCTION,
                                             TenuredObject);
        if (!dummyFunction_)
            return false;

        ScriptedCaller scriptedCaller;
        if (parser_.ss->filename()) {
            scriptedCaller.line = scriptedCaller.column = 0;  // unused
            scriptedCaller.filename = DuplicateString(parser_.ss->filename());
            if (!scriptedCaller.filename)
                return false;
        }

        CompileArgs args;
        if (!args.initFromContext(cx_, Move(scriptedCaller)))
            return false;

        auto env = MakeUnique<ModuleEnvironment>(ModuleKind::AsmJS);
        if (!env ||
            !env->sigs.resize(AsmJSMaxTypes) ||
            !env->funcSigs.resize(AsmJSMaxFuncs) ||
            !env->funcImportGlobalDataOffsets.resize(AsmJSMaxImports) ||
            !env->tables.resize(AsmJSMaxTables) ||
            !env->asmJSSigToTableIndex.resize(AsmJSMaxTypes))
        {
            return false;
        }

        env->minMemoryLength = RoundUpToNextValidAsmJSHeapLength(0);

        if (!mg_.init(Move(env), args, asmJSMetadata_.get()))
            return false;

        return true;
    }

    ExclusiveContext* cx() const             { return cx_; }
    PropertyName* moduleFunctionName() const { return moduleFunctionName_; }
    PropertyName* globalArgumentName() const { return globalArgumentName_; }
    PropertyName* importArgumentName() const { return importArgumentName_; }
    PropertyName* bufferArgumentName() const { return bufferArgumentName_; }
    ModuleGenerator& mg()                    { return mg_; }
    AsmJSParser& parser() const              { return parser_; }
    TokenStream& tokenStream() const         { return parser_.tokenStream; }
    RootedFunction& dummyFunction()          { return dummyFunction_; }
    bool atomicsPresent() const              { return atomicsPresent_; }
    uint32_t minMemoryLength() const         { return mg_.minMemoryLength(); }

    void initModuleFunctionName(PropertyName* name) {
        MOZ_ASSERT(!moduleFunctionName_);
        moduleFunctionName_ = name;
    }
    MOZ_MUST_USE bool initGlobalArgumentName(PropertyName* n) {
        MOZ_ASSERT(n->isTenured());
        globalArgumentName_ = n;
        if (n) {
            asmJSMetadata_->globalArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->globalArgumentName)
                return false;
        }
        return true;
    }
    MOZ_MUST_USE bool initImportArgumentName(PropertyName* n) {
        MOZ_ASSERT(n->isTenured());
        importArgumentName_ = n;
        if (n) {
            asmJSMetadata_->importArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->importArgumentName)
                return false;
        }
        return true;
    }
    MOZ_MUST_USE bool initBufferArgumentName(PropertyName* n) {
        MOZ_ASSERT(n->isTenured());
        bufferArgumentName_ = n;
        if (n) {
            asmJSMetadata_->bufferArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->bufferArgumentName)
                return false;
        }
        return true;
    }
    bool addGlobalVarInit(PropertyName* var, const NumLit& lit, Type type, bool isConst) {
        MOZ_ASSERT(type.isGlobalVarType());
        MOZ_ASSERT(type == Type::canonicalize(Type::lit(lit)));

        uint32_t index;
        if (!mg_.addGlobal(type.canonicalToValType(), isConst, &index))
            return false;

        Global::Which which = isConst ? Global::ConstantLiteral : Global::Variable;
        Global* global = validationLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = (isConst ? Type::lit(lit) : type).which();
        if (isConst)
            global->u.varOrConst.literalValue_ = lit;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Variable, nullptr);
        g.pod.u.var.initKind_ = AsmJSGlobal::InitConstant;
        g.pod.u.var.u.val_ = lit.value();
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addGlobalVarImport(PropertyName* var, PropertyName* field, Type type, bool isConst) {
        MOZ_ASSERT(type.isGlobalVarType());

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        uint32_t index;
        ValType valType = type.canonicalToValType();
        if (!mg_.addGlobal(valType, isConst, &index))
            return false;

        Global::Which which = isConst ? Global::ConstantImport : Global::Variable;
        Global* global = validationLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = type.which();
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Variable, Move(fieldChars));
        g.pod.u.var.initKind_ = AsmJSGlobal::InitImport;
        g.pod.u.var.u.importType_ = valType;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addArrayView(PropertyName* var, Scalar::Type vt, PropertyName* maybeField) {
        UniqueChars fieldChars;
        if (maybeField) {
            fieldChars = StringToNewUTF8CharsZ(cx_, *maybeField);
            if (!fieldChars)
                return false;
        }

        if (!arrayViews_.append(ArrayView(var, vt)))
            return false;

        Global* global = validationLifo_.new_<Global>(Global::ArrayView);
        if (!global)
            return false;
        global->u.viewInfo.viewType_ = vt;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::ArrayView, Move(fieldChars));
        g.pod.u.viewType_ = vt;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addMathBuiltinFunction(PropertyName* var, AsmJSMathBuiltinFunction func,
                                PropertyName* field)
    {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::MathBuiltinFunction);
        if (!global)
            return false;
        global->u.mathBuiltinFunc_ = func;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::MathBuiltinFunction, Move(fieldChars));
        g.pod.u.mathBuiltinFunc_ = func;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
  private:
    bool addGlobalDoubleConstant(PropertyName* var, double constant) {
        Global* global = validationLifo_.new_<Global>(Global::ConstantLiteral);
        if (!global)
            return false;
        global->u.varOrConst.type_ = Type::Double;
        global->u.varOrConst.literalValue_ = NumLit(NumLit::Double, DoubleValue(constant));
        return globalMap_.putNew(var, global);
    }
  public:
    bool addMathBuiltinConstant(PropertyName* var, double constant, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (!addGlobalDoubleConstant(var, constant))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Constant, Move(fieldChars));
        g.pod.u.constant.value_ = constant;
        g.pod.u.constant.kind_ = AsmJSGlobal::MathConstant;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addGlobalConstant(PropertyName* var, double constant, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (!addGlobalDoubleConstant(var, constant))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Constant, Move(fieldChars));
        g.pod.u.constant.value_ = constant;
        g.pod.u.constant.kind_ = AsmJSGlobal::GlobalConstant;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addAtomicsBuiltinFunction(PropertyName* var, AsmJSAtomicsBuiltinFunction func,
                                   PropertyName* field)
    {
        if (!JitOptions.asmJSAtomicsEnable)
            return failCurrentOffset("asm.js Atomics only enabled in wasm test mode");

        atomicsPresent_ = true;

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::AtomicsBuiltinFunction);
        if (!global)
            return false;
        global->u.atomicsBuiltinFunc_ = func;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::AtomicsBuiltinFunction, Move(fieldChars));
        g.pod.u.atomicsBuiltinFunc_ = func;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addArrayViewCtor(PropertyName* var, Scalar::Type vt, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::ArrayViewCtor);
        if (!global)
            return false;
        global->u.viewInfo.viewType_ = vt;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::ArrayViewCtor, Move(fieldChars));
        g.pod.u.viewType_ = vt;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addFFI(PropertyName* var, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (asmJSMetadata_->numFFIs == UINT32_MAX)
            return false;
        uint32_t ffiIndex = asmJSMetadata_->numFFIs++;

        Global* global = validationLifo_.new_<Global>(Global::FFI);
        if (!global)
            return false;
        global->u.ffiIndex_ = ffiIndex;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::FFI, Move(fieldChars));
        g.pod.u.ffiIndex_ = ffiIndex;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addExportField(ParseNode* pn, const Func& func, PropertyName* maybeField) {
        // Record the field name of this export.
        CacheableChars fieldChars;
        if (maybeField)
            fieldChars = StringToNewUTF8CharsZ(cx_, *maybeField);
        else
            fieldChars = DuplicateString("");
        if (!fieldChars)
            return false;

        // Declare which function is exported which gives us an index into the
        // module ExportVector.
        if (!mg_.addExport(Move(fieldChars), func.index()))
            return false;

        // The exported function might have already been exported in which case
        // the index will refer into the range of AsmJSExports.
        return asmJSMetadata_->asmJSExports.emplaceBack(func.index(),
                                                        func.srcBegin() - asmJSMetadata_->srcStart,
                                                        func.srcEnd() - asmJSMetadata_->srcStart);
    }
    bool addFunction(PropertyName* name, uint32_t firstUse, Sig&& sig, Func** func) {
        uint32_t sigIndex;
        if (!declareSig(Move(sig), &sigIndex))
            return false;
        uint32_t funcIndex = AsmJSFirstDefFuncIndex + numFunctions();
        if (funcIndex >= AsmJSMaxFuncs)
            return failCurrentOffset("too many functions");
        mg_.initFuncSig(funcIndex, sigIndex);
        Global* global = validationLifo_.new_<Global>(Global::Function);
        if (!global)
            return false;
        global->u.funcIndex_ = funcIndex;
        if (!globalMap_.putNew(name, global))
            return false;
        *func = validationLifo_.new_<Func>(name, firstUse, funcIndex);
        return *func && functions_.append(*func);
    }
    bool declareFuncPtrTable(Sig&& sig, PropertyName* name, uint32_t firstUse, uint32_t mask,
                             uint32_t* index)
    {
        if (mask > MaxTableLength)
            return failCurrentOffset("function pointer table too big");
        uint32_t sigIndex;
        if (!newSig(Move(sig), &sigIndex))
            return false;
        if (!mg_.initSigTableLength(sigIndex, mask + 1))
            return false;
        Global* global = validationLifo_.new_<Global>(Global::FuncPtrTable);
        if (!global)
            return false;
        global->u.funcPtrTableIndex_ = *index = funcPtrTables_.length();
        if (!globalMap_.putNew(name, global))
            return false;
        FuncPtrTable* t = validationLifo_.new_<FuncPtrTable>(sigIndex, name, firstUse, mask);
        return t && funcPtrTables_.append(t);
    }
    bool defineFuncPtrTable(uint32_t funcPtrTableIndex, Uint32Vector&& elems) {
        FuncPtrTable& table = *funcPtrTables_[funcPtrTableIndex];
        if (table.defined())
            return false;
        table.define();
        return mg_.initSigTableElems(table.sigIndex(), Move(elems));
    }
    bool declareImport(PropertyName* name, Sig&& sig, unsigned ffiIndex, uint32_t* funcIndex) {
        ImportMap::AddPtr p = importMap_.lookupForAdd(NamedSig::Lookup(name, sig));
        if (p) {
            *funcIndex = p->value();
            return true;
        }
        *funcIndex = asmJSMetadata_->asmJSImports.length();
        if (*funcIndex > AsmJSMaxImports)
            return failCurrentOffset("too many imports");
        if (!asmJSMetadata_->asmJSImports.emplaceBack(ffiIndex))
            return false;
        uint32_t sigIndex;
        if (!declareSig(Move(sig), &sigIndex))
            return false;
        if (!mg_.initImport(*funcIndex, sigIndex))
            return false;
        return importMap_.add(p, NamedSig(name, mg_.sig(sigIndex)), *funcIndex);
    }

    bool tryConstantAccess(uint64_t start, uint64_t width) {
        MOZ_ASSERT(UINT64_MAX - start > width);
        uint64_t len = start + width;
        if (len > uint64_t(INT32_MAX) + 1)
            return false;
        len = RoundUpToNextValidAsmJSHeapLength(len);
        if (len > mg_.minMemoryLength())
            mg_.bumpMinMemoryLength(len);
        return true;
    }

    // Error handling.
    bool hasAlreadyFailed() const {
        return !!errorString_;
    }

    bool failOffset(uint32_t offset, const char* str) {
        MOZ_ASSERT(!hasAlreadyFailed());
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(str);
        errorOffset_ = offset;
        errorString_ = DuplicateString(str);
        return false;
    }

    bool failCurrentOffset(const char* str) {
        return failOffset(tokenStream().currentToken().pos.begin, str);
    }

    bool fail(ParseNode* pn, const char* str) {
        return failOffset(pn->pn_pos.begin, str);
    }

    bool failfVAOffset(uint32_t offset, const char* fmt, va_list ap) {
        MOZ_ASSERT(!hasAlreadyFailed());
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(fmt);
        errorOffset_ = offset;
        errorString_.reset(JS_vsmprintf(fmt, ap));
        return false;
    }

    bool failfOffset(uint32_t offset, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        failfVAOffset(offset, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failf(ParseNode* pn, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        failfVAOffset(pn->pn_pos.begin, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failNameOffset(uint32_t offset, const char* fmt, PropertyName* name) {
        // This function is invoked without the caller properly rooting its locals.
        gc::AutoSuppressGC suppress(cx_);
        JSAutoByteString bytes;
        if (AtomToPrintableString(cx_, name, &bytes))
            failfOffset(offset, fmt, bytes.ptr());
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name) {
        return failNameOffset(pn->pn_pos.begin, fmt, name);
    }

    bool failOverRecursed() {
        errorOverRecursed_ = true;
        return false;
    }

    unsigned numArrayViews() const {
        return arrayViews_.length();
    }
    const ArrayView& arrayView(unsigned i) const {
        return arrayViews_[i];
    }
    unsigned numFunctions() const {
        return functions_.length();
    }
    Func& function(unsigned i) const {
        return *functions_[i];
    }
    unsigned numFuncPtrTables() const {
        return funcPtrTables_.length();
    }
    FuncPtrTable& funcPtrTable(unsigned i) const {
        return *funcPtrTables_[i];
    }

    const Global* lookupGlobal(PropertyName* name) const {
        if (GlobalMap::Ptr p = globalMap_.lookup(name))
            return p->value();
        return nullptr;
    }

    Func* lookupFunction(PropertyName* name) {
        if (GlobalMap::Ptr p = globalMap_.lookup(name)) {
            Global* value = p->value();
            if (value->which() == Global::Function) {
                MOZ_ASSERT(value->funcIndex() >= AsmJSFirstDefFuncIndex);
                return functions_[value->funcIndex() - AsmJSFirstDefFuncIndex];
            }
        }
        return nullptr;
    }

    bool lookupStandardLibraryMathName(PropertyName* name, MathBuiltin* mathBuiltin) const {
        if (MathNameMap::Ptr p = standardLibraryMathNames_.lookup(name)) {
            *mathBuiltin = p->value();
            return true;
        }
        return false;
    }
    bool lookupStandardLibraryAtomicsName(PropertyName* name, AsmJSAtomicsBuiltinFunction* atomicsBuiltin) const {
        if (AtomicsNameMap::Ptr p = standardLibraryAtomicsNames_.lookup(name)) {
            *atomicsBuiltin = p->value();
            return true;
        }
        return false;
    }

    bool startFunctionBodies() {
        if (!arrayViews_.empty())
            mg_.initMemoryUsage(atomicsPresent_ ? MemoryUsage::Shared : MemoryUsage::Unshared);

        return mg_.startFuncDefs();
    }
    bool finishFunctionBodies() {
        return mg_.finishFuncDefs();
    }
    SharedModule finish() {
        MOZ_ASSERT(asmJSMetadata_->asmJSFuncNames.empty());
        for (const Func* func : functions_) {
            CacheableChars funcName = StringToNewUTF8CharsZ(cx_, *func->name());
            if (!funcName || !asmJSMetadata_->asmJSFuncNames.emplaceBack(Move(funcName)))
                return nullptr;
        }

        uint32_t endBeforeCurly = tokenStream().currentToken().pos.end;
        asmJSMetadata_->srcLength = endBeforeCurly - asmJSMetadata_->srcStart;

        TokenPos pos;
        JS_ALWAYS_TRUE(tokenStream().peekTokenPos(&pos, TokenStream::Operand));
        uint32_t endAfterCurly = pos.end;
        asmJSMetadata_->srcLengthWithRightBrace = endAfterCurly - asmJSMetadata_->srcStart;

        // asm.js does not have any wasm bytecode to save; view-source is
        // provided through the ScriptSource.
        SharedBytes bytes = js_new<ShareableBytes>();
        if (!bytes)
            return nullptr;

        return mg_.finish(*bytes);
    }
};

/*****************************************************************************/
// Numeric literal utilities

static bool
IsNumericNonFloatLiteral(ParseNode* pn)
{
    // Note: '-' is never rolled into the number; numbers are always positive
    // and negations must be applied manually.
    return pn->isKind(PNK_NUMBER) ||
           (pn->isKind(PNK_NEG) && UnaryKid(pn)->isKind(PNK_NUMBER));
}

static bool
IsCallToGlobal(ModuleValidator& m, ParseNode* pn, const ModuleValidator::Global** global)
{
    if (!pn->isKind(PNK_CALL))
        return false;

    ParseNode* callee = CallCallee(pn);
    if (!callee->isKind(PNK_NAME))
        return false;

    *global = m.lookupGlobal(callee->name());
    return !!*global;
}

static bool
IsCoercionCall(ModuleValidator& m, ParseNode* pn, Type* coerceTo, ParseNode** coercedExpr)
{
    const ModuleValidator::Global* global;
    if (!IsCallToGlobal(m, pn, &global))
        return false;

    if (CallArgListLength(pn) != 1)
        return false;

    if (coercedExpr)
        *coercedExpr = CallArgList(pn);

    if (global->isMathFunction() && global->mathBuiltinFunction() == AsmJSMathBuiltin_fround) {
        *coerceTo = Type::Float;
        return true;
    }

    return false;
}

static bool
IsFloatLiteral(ModuleValidator& m, ParseNode* pn)
{
    ParseNode* coercedExpr;
    Type coerceTo;
    if (!IsCoercionCall(m, pn, &coerceTo, &coercedExpr))
        return false;
    // Don't fold into || to avoid clang/memcheck bug (bug 1077031).
    if (!coerceTo.isFloat())
        return false;
    return IsNumericNonFloatLiteral(coercedExpr);
}

static bool
IsNumericLiteral(ModuleValidator& m, ParseNode* pn);

static NumLit
ExtractNumericLiteral(ModuleValidator& m, ParseNode* pn);

static inline bool
IsLiteralInt(ModuleValidator& m, ParseNode* pn, uint32_t* u32);

static bool
IsNumericLiteral(ModuleValidator& m, ParseNode* pn)
{
    if (IsNumericNonFloatLiteral(pn) || IsFloatLiteral(m, pn))
        return true;
    return false;
}

// The JS grammar treats -42 as -(42) (i.e., with separate grammar
// productions) for the unary - and literal 42). However, the asm.js spec
// recognizes -42 (modulo parens, so -(42) and -((42))) as a single literal
// so fold the two potential parse nodes into a single double value.
static double
ExtractNumericNonFloatValue(ParseNode* pn, ParseNode** out = nullptr)
{
    MOZ_ASSERT(IsNumericNonFloatLiteral(pn));

    if (pn->isKind(PNK_NEG)) {
        pn = UnaryKid(pn);
        if (out)
            *out = pn;
        return -NumberNodeValue(pn);
    }

    return NumberNodeValue(pn);
}

static NumLit
ExtractNumericLiteral(ModuleValidator& m, ParseNode* pn)
{
    MOZ_ASSERT(IsNumericLiteral(m, pn));

    if (pn->isKind(PNK_CALL)) {
        // Float literals are explicitly coerced and thus the coerced literal may be
        // any valid (non-float) numeric literal.
        MOZ_ASSERT((CallArgListLength(pn) == 1));
        pn = CallArgList(pn);
        double d = ExtractNumericNonFloatValue(pn);
        return NumLit(NumLit::Float, DoubleValue(d));
    }

    double d = ExtractNumericNonFloatValue(pn, &pn);

    // The asm.js spec syntactically distinguishes any literal containing a
    // decimal point or the literal -0 as having double type.
    if (NumberNodeHasFrac(pn) || IsNegativeZero(d))
        return NumLit(NumLit::Double, DoubleValue(d));

    // The syntactic checks above rule out these double values.
    MOZ_ASSERT(!IsNegativeZero(d));
    MOZ_ASSERT(!IsNaN(d));

    // Although doubles can only *precisely* represent 53-bit integers, they
    // can *imprecisely* represent integers much bigger than an int64_t.
    // Furthermore, d may be inf or -inf. In both cases, casting to an int64_t
    // is undefined, so test against the integer bounds using doubles.
    if (d < double(INT32_MIN) || d > double(UINT32_MAX))
        return NumLit(NumLit::OutOfRangeInt, UndefinedValue());

    // With the above syntactic and range limitations, d is definitely an
    // integer in the range [INT32_MIN, UINT32_MAX] range.
    int64_t i64 = int64_t(d);
    if (i64 >= 0) {
        if (i64 <= INT32_MAX)
            return NumLit(NumLit::Fixnum, Int32Value(i64));
        MOZ_ASSERT(i64 <= UINT32_MAX);
        return NumLit(NumLit::BigUnsigned, Int32Value(uint32_t(i64)));
    }
    MOZ_ASSERT(i64 >= INT32_MIN);
    return NumLit(NumLit::NegativeInt, Int32Value(i64));
}

static inline bool
IsLiteralInt(const NumLit& lit, uint32_t* u32)
{
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::BigUnsigned:
      case NumLit::NegativeInt:
        *u32 = lit.toUint32();
        return true;
      case NumLit::Double:
      case NumLit::Float:
      case NumLit::OutOfRangeInt:
        return false;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal type");
}

static inline bool
IsLiteralInt(ModuleValidator& m, ParseNode* pn, uint32_t* u32)
{
    return IsNumericLiteral(m, pn) &&
           IsLiteralInt(ExtractNumericLiteral(m, pn), u32);
}

/*****************************************************************************/

namespace {

typedef Vector<PropertyName*, 4, SystemAllocPolicy> NameVector;

// Encapsulates the building of an asm bytecode function from an asm.js function
// source code, packing the asm.js code into the asm bytecode form that can
// be decoded and compiled with a FunctionCompiler.
class MOZ_STACK_CLASS FunctionValidator
{
  public:
    struct Local
    {
        Type type;
        unsigned slot;
        Local(Type t, unsigned slot) : type(t), slot(slot) {
            MOZ_ASSERT(type.isCanonicalValType());
        }
    };

  private:
    typedef HashMap<PropertyName*, Local> LocalMap;
    typedef HashMap<PropertyName*, uint32_t> LabelMap;

    ModuleValidator&  m_;
    ParseNode*        fn_;

    FunctionGenerator fg_;
    Maybe<Encoder>    encoder_;

    LocalMap          locals_;

    // Labels
    LabelMap          breakLabels_;
    LabelMap          continueLabels_;
    Uint32Vector      breakableStack_;
    Uint32Vector      continuableStack_;
    uint32_t          blockDepth_;

    bool              hasAlreadyReturned_;
    ExprType          ret_;

  public:
    FunctionValidator(ModuleValidator& m, ParseNode* fn)
      : m_(m),
        fn_(fn),
        locals_(m.cx()),
        breakLabels_(m.cx()),
        continueLabels_(m.cx()),
        blockDepth_(0),
        hasAlreadyReturned_(false),
        ret_(ExprType::Limit)
    {}

    ModuleValidator& m() const        { return m_; }
    ExclusiveContext* cx() const      { return m_.cx(); }
    ParseNode* fn() const             { return fn_; }

    bool init(PropertyName* name, unsigned line) {
        if (!locals_.init() || !breakLabels_.init() || !continueLabels_.init())
            return false;

        if (!m_.mg().startFuncDef(line, &fg_))
            return false;

        encoder_.emplace(fg_.bytes());
        return true;
    }

    bool finish(uint32_t funcIndex) {
        MOZ_ASSERT(!blockDepth_);
        MOZ_ASSERT(breakableStack_.empty());
        MOZ_ASSERT(continuableStack_.empty());
        MOZ_ASSERT(breakLabels_.empty());
        MOZ_ASSERT(continueLabels_.empty());

        return m_.mg().finishFuncDef(funcIndex, &fg_);
    }

    bool fail(ParseNode* pn, const char* str) {
        return m_.fail(pn, str);
    }

    bool failf(ParseNode* pn, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        m_.failfVAOffset(pn->pn_pos.begin, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name) {
        return m_.failName(pn, fmt, name);
    }

    /***************************************************** Attributes */

    void setUsesAtomics() {
        fg_.setUsesAtomics();
    }

    /***************************************************** Local scope setup */

    bool addLocal(ParseNode* pn, PropertyName* name, Type type) {
        LocalMap::AddPtr p = locals_.lookupForAdd(name);
        if (p)
            return failName(pn, "duplicate local name '%s' not allowed", name);
        return locals_.add(p, name, Local(type, locals_.count()));
    }

    /****************************** For consistency of returns in a function */

    bool hasAlreadyReturned() const {
        return hasAlreadyReturned_;
    }

    ExprType returnedType() const {
        return ret_;
    }

    void setReturnedType(ExprType ret) {
        ret_ = ret;
        hasAlreadyReturned_ = true;
    }

    /**************************************************************** Labels */
  private:
    bool writeBr(uint32_t absolute, Op op = Op::Br) {
        MOZ_ASSERT(op == Op::Br || op == Op::BrIf);
        MOZ_ASSERT(absolute < blockDepth_);
        return encoder().writeOp(op) &&
               encoder().writeVarU32(blockDepth_ - 1 - absolute);
    }
    void removeLabel(PropertyName* label, LabelMap* map) {
        LabelMap::Ptr p = map->lookup(label);
        MOZ_ASSERT(p);
        map->remove(p);
    }

  public:
    bool pushBreakableBlock() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               breakableStack_.append(blockDepth_++);
    }
    bool popBreakableBlock() {
        JS_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End);
    }

    bool pushUnbreakableBlock(const NameVector* labels = nullptr) {
        if (labels) {
            for (PropertyName* label : *labels) {
                if (!breakLabels_.putNew(label, blockDepth_))
                    return false;
            }
        }
        blockDepth_++;
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void));
    }
    bool popUnbreakableBlock(const NameVector* labels = nullptr) {
        if (labels) {
            for (PropertyName* label : *labels)
                removeLabel(label, &breakLabels_);
        }
        --blockDepth_;
        return encoder().writeOp(Op::End);
    }

    bool pushContinuableBlock() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               continuableStack_.append(blockDepth_++);
    }
    bool popContinuableBlock() {
        JS_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End);
    }

    bool pushLoop() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               encoder().writeOp(Op::Loop) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               breakableStack_.append(blockDepth_++) &&
               continuableStack_.append(blockDepth_++);
    }
    bool popLoop() {
        JS_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
        JS_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End) &&
               encoder().writeOp(Op::End);
    }

    bool pushIf(size_t* typeAt) {
        ++blockDepth_;
        return encoder().writeOp(Op::If) &&
               encoder().writePatchableFixedU7(typeAt);
    }
    bool switchToElse() {
        MOZ_ASSERT(blockDepth_ > 0);
        return encoder().writeOp(Op::Else);
    }
    void setIfType(size_t typeAt, ExprType type) {
        encoder().patchFixedU7(typeAt, uint8_t(type));
    }
    bool popIf() {
        MOZ_ASSERT(blockDepth_ > 0);
        --blockDepth_;
        return encoder().writeOp(Op::End);
    }
    bool popIf(size_t typeAt, ExprType type) {
        MOZ_ASSERT(blockDepth_ > 0);
        --blockDepth_;
        if (!encoder().writeOp(Op::End))
            return false;

        setIfType(typeAt, type);
        return true;
    }

    bool writeBreakIf() {
        return writeBr(breakableStack_.back(), Op::BrIf);
    }
    bool writeContinueIf() {
        return writeBr(continuableStack_.back(), Op::BrIf);
    }
    bool writeUnlabeledBreakOrContinue(bool isBreak) {
        return writeBr(isBreak? breakableStack_.back() : continuableStack_.back());
    }
    bool writeContinue() {
        return writeBr(continuableStack_.back());
    }

    bool addLabels(const NameVector& labels, uint32_t relativeBreakDepth,
                   uint32_t relativeContinueDepth)
    {
        for (PropertyName* label : labels) {
            if (!breakLabels_.putNew(label, blockDepth_ + relativeBreakDepth))
                return false;
            if (!continueLabels_.putNew(label, blockDepth_ + relativeContinueDepth))
                return false;
        }
        return true;
    }
    void removeLabels(const NameVector& labels) {
        for (PropertyName* label : labels) {
            removeLabel(label, &breakLabels_);
            removeLabel(label, &continueLabels_);
        }
    }
    bool writeLabeledBreakOrContinue(PropertyName* label, bool isBreak) {
        LabelMap& map = isBreak ? breakLabels_ : continueLabels_;
        if (LabelMap::Ptr p = map.lookup(label))
            return writeBr(p->value());
        MOZ_CRASH("nonexistent label");
    }

    /*************************************************** Read-only interface */

    const Local* lookupLocal(PropertyName* name) const {
        if (auto p = locals_.lookup(name))
            return &p->value();
        return nullptr;
    }

    const ModuleValidator::Global* lookupGlobal(PropertyName* name) const {
        if (locals_.has(name))
            return nullptr;
        return m_.lookupGlobal(name);
    }

    size_t numLocals() const { return locals_.count(); }

    /**************************************************** Encoding interface */

    Encoder& encoder() { return *encoder_; }

    MOZ_MUST_USE bool writeInt32Lit(int32_t i32) {
        return encoder().writeOp(Op::I32Const) &&
               encoder().writeVarS32(i32);
    }
    MOZ_MUST_USE bool writeConstExpr(const NumLit& lit) {
        switch (lit.which()) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return writeInt32Lit(lit.toInt32());
          case NumLit::Float:
            return encoder().writeOp(Op::F32Const) &&
                   encoder().writeFixedF32(lit.toFloat());
          case NumLit::Double:
            return encoder().writeOp(Op::F64Const) &&
                   encoder().writeFixedF64(lit.toDouble());
          case NumLit::OutOfRangeInt:
            break;
        }
        MOZ_CRASH("unexpected literal type");
    }
    MOZ_MUST_USE bool writeCall(ParseNode* pn, Op op) {
        return encoder().writeOp(op) &&
               fg_.addCallSiteLineNum(m().tokenStream().srcCoords.lineNum(pn->pn_pos.begin));
    }
    MOZ_MUST_USE bool prepareCall(ParseNode* pn) {
        return fg_.addCallSiteLineNum(m().tokenStream().srcCoords.lineNum(pn->pn_pos.begin));
    }
};

} /* anonymous namespace */

/*****************************************************************************/
// asm.js type-checking and code-generation algorithm

static bool
CheckIdentifier(ModuleValidator& m, ParseNode* usepn, PropertyName* name)
{
    if (name == m.cx()->names().arguments || name == m.cx()->names().eval)
        return m.failName(usepn, "'%s' is not an allowed identifier", name);
    return true;
}

static bool
CheckModuleLevelName(ModuleValidator& m, ParseNode* usepn, PropertyName* name)
{
    if (!CheckIdentifier(m, usepn, name))
        return false;

    if (name == m.moduleFunctionName() ||
        name == m.globalArgumentName() ||
        name == m.importArgumentName() ||
        name == m.bufferArgumentName() ||
        m.lookupGlobal(name))
    {
        return m.failName(usepn, "duplicate name '%s' not allowed", name);
    }

    return true;
}

static bool
CheckFunctionHead(ModuleValidator& m, FunctionNode* funNode)
{
    FunctionBox* funbox = funNode->funbox();
    MOZ_ASSERT(!funbox->isExprBody());

    if (funbox->hasRest())
        return m.fail(funNode, "rest args not allowed");
    if (funbox->hasDestructuringArgs)
        return m.fail(funNode, "destructuring args not allowed");
    return true;
}

static bool
CheckArgument(ModuleValidator& m, ParseNode* arg, PropertyName** name)
{
    *name = nullptr;

    if (!arg->isKind(PNK_NAME))
        return m.fail(arg, "argument is not a plain name");

    if (!CheckIdentifier(m, arg, arg->name()))
        return false;

    *name = arg->name();
    return true;
}

static bool
CheckModuleArgument(ModuleValidator& m, ParseNode* arg, PropertyName** name)
{
    if (!CheckArgument(m, arg, name))
        return false;

    if (!CheckModuleLevelName(m, arg, *name))
        return false;

    return true;
}

static bool
CheckModuleArguments(ModuleValidator& m, FunctionNode* funNode)
{
    unsigned numFormals;
    ParseNode* arg1 = FunctionFormalParametersList(funNode, &numFormals);
    ParseNode* arg2 = arg1 ? NextNode(arg1) : nullptr;
    ParseNode* arg3 = arg2 ? NextNode(arg2) : nullptr;

    if (numFormals > 3)
        return m.fail(funNode, "asm.js modules takes at most 3 argument");

    PropertyName* arg1Name = nullptr;
    if (arg1 && !CheckModuleArgument(m, arg1, &arg1Name))
        return false;
    if (!m.initGlobalArgumentName(arg1Name))
        return false;

    PropertyName* arg2Name = nullptr;
    if (arg2 && !CheckModuleArgument(m, arg2, &arg2Name))
        return false;
    if (!m.initImportArgumentName(arg2Name))
        return false;

    PropertyName* arg3Name = nullptr;
    if (arg3 && !CheckModuleArgument(m, arg3, &arg3Name))
        return false;
    if (!m.initBufferArgumentName(arg3Name))
        return false;

    return true;
}

static bool
CheckPrecedingStatements(ModuleValidator& m, ParseNode* stmtList)
{
    MOZ_ASSERT(stmtList->isKind(PNK_STATEMENTLIST));

    ParseNode* stmt = ListHead(stmtList);
    for (unsigned i = 0, n = ListLength(stmtList); i < n; i++) {
        if (!IsIgnoredDirective(m.cx(), stmt))
            return m.fail(stmt, "invalid asm.js statement");
    }

    return true;
}

static bool
CheckGlobalVariableInitConstant(ModuleValidator& m, PropertyName* varName, ParseNode* initNode,
                                bool isConst)
{
    NumLit lit = ExtractNumericLiteral(m, initNode);
    if (!lit.valid())
        return m.fail(initNode, "global initializer is out of representable integer range");

    Type canonicalType = Type::canonicalize(Type::lit(lit));
    if (!canonicalType.isGlobalVarType())
        return m.fail(initNode, "global variable type not allowed");

    return m.addGlobalVarInit(varName, lit, canonicalType, isConst);
}

static bool
CheckTypeAnnotation(ModuleValidator& m, ParseNode* coercionNode, Type* coerceTo,
                    ParseNode** coercedExpr = nullptr)
{
    switch (coercionNode->getKind()) {
      case PNK_BITOR: {
        ParseNode* rhs = BitwiseRight(coercionNode);
        uint32_t i;
        if (!IsLiteralInt(m, rhs, &i) || i != 0)
            return m.fail(rhs, "must use |0 for argument/return coercion");
        *coerceTo = Type::Int;
        if (coercedExpr)
            *coercedExpr = BitwiseLeft(coercionNode);
        return true;
      }
      case PNK_POS: {
        *coerceTo = Type::Double;
        if (coercedExpr)
            *coercedExpr = UnaryKid(coercionNode);
        return true;
      }
      case PNK_CALL: {
        if (IsCoercionCall(m, coercionNode, coerceTo, coercedExpr))
            return true;
        break;
      }
      default:;
    }

    return m.fail(coercionNode, "must be of the form +x, x|0 or fround(x)");
}

static bool
CheckGlobalVariableInitImport(ModuleValidator& m, PropertyName* varName, ParseNode* initNode,
                              bool isConst)
{
    Type coerceTo;
    ParseNode* coercedExpr;
    if (!CheckTypeAnnotation(m, initNode, &coerceTo, &coercedExpr))
        return false;

    if (!coercedExpr->isKind(PNK_DOT))
        return m.failName(coercedExpr, "invalid import expression for global '%s'", varName);

    if (!coerceTo.isGlobalVarType())
        return m.fail(initNode, "global variable type not allowed");

    ParseNode* base = DotBase(coercedExpr);
    PropertyName* field = DotMember(coercedExpr);

    PropertyName* importName = m.importArgumentName();
    if (!importName)
        return m.fail(coercedExpr, "cannot import without an asm.js foreign parameter");
    if (!IsUseOfName(base, importName))
        return m.failName(coercedExpr, "base of import expression must be '%s'", importName);

    return m.addGlobalVarImport(varName, field, coerceTo, isConst);
}

static bool
IsArrayViewCtorName(ModuleValidator& m, PropertyName* name, Scalar::Type* type)
{
    JSAtomState& names = m.cx()->names();
    if (name == names.Int8Array) {
        *type = Scalar::Int8;
    } else if (name == names.Uint8Array) {
        *type = Scalar::Uint8;
    } else if (name == names.Int16Array) {
        *type = Scalar::Int16;
    } else if (name == names.Uint16Array) {
        *type = Scalar::Uint16;
    } else if (name == names.Int32Array) {
        *type = Scalar::Int32;
    } else if (name == names.Uint32Array) {
        *type = Scalar::Uint32;
    } else if (name == names.Float32Array) {
        *type = Scalar::Float32;
    } else if (name == names.Float64Array) {
        *type = Scalar::Float64;
    } else {
        return false;
    }
    return true;
}

static bool
CheckNewArrayViewArgs(ModuleValidator& m, ParseNode* newExpr, PropertyName* bufferName)
{
    ParseNode* ctorExpr = BinaryLeft(newExpr);
    ParseNode* ctorArgs = BinaryRight(newExpr);
    ParseNode* bufArg = ListHead(ctorArgs);
    if (!bufArg || NextNode(bufArg) != nullptr)
        return m.fail(ctorExpr, "array view constructor takes exactly one argument");

    if (!IsUseOfName(bufArg, bufferName))
        return m.failName(bufArg, "argument to array view constructor must be '%s'", bufferName);

    return true;
}

static bool
CheckNewArrayView(ModuleValidator& m, PropertyName* varName, ParseNode* newExpr)
{
    PropertyName* globalName = m.globalArgumentName();
    if (!globalName)
        return m.fail(newExpr, "cannot create array view without an asm.js global parameter");

    PropertyName* bufferName = m.bufferArgumentName();
    if (!bufferName)
        return m.fail(newExpr, "cannot create array view without an asm.js heap parameter");

    ParseNode* ctorExpr = BinaryLeft(newExpr);

    PropertyName* field;
    Scalar::Type type;
    if (ctorExpr->isKind(PNK_DOT)) {
        ParseNode* base = DotBase(ctorExpr);

        if (!IsUseOfName(base, globalName))
            return m.failName(base, "expecting '%s.*Array", globalName);

        field = DotMember(ctorExpr);
        if (!IsArrayViewCtorName(m, field, &type))
            return m.fail(ctorExpr, "could not match typed array name");
    } else {
        if (!ctorExpr->isKind(PNK_NAME))
            return m.fail(ctorExpr, "expecting name of imported array view constructor");

        PropertyName* globalName = ctorExpr->name();
        const ModuleValidator::Global* global = m.lookupGlobal(globalName);
        if (!global)
            return m.failName(ctorExpr, "%s not found in module global scope", globalName);

        if (global->which() != ModuleValidator::Global::ArrayViewCtor)
            return m.failName(ctorExpr, "%s must be an imported array view constructor", globalName);

        field = nullptr;
        type = global->viewType();
    }

    if (!CheckNewArrayViewArgs(m, newExpr, bufferName))
        return false;

    return m.addArrayView(varName, type, field);
}

static bool
CheckGlobalMathImport(ModuleValidator& m, ParseNode* initNode, PropertyName* varName,
                      PropertyName* field)
{
    // Math builtin, with the form glob.Math.[[builtin]]
    ModuleValidator::MathBuiltin mathBuiltin;
    if (!m.lookupStandardLibraryMathName(field, &mathBuiltin))
        return m.failName(initNode, "'%s' is not a standard Math builtin", field);

    switch (mathBuiltin.kind) {
      case ModuleValidator::MathBuiltin::Function:
        return m.addMathBuiltinFunction(varName, mathBuiltin.u.func, field);
      case ModuleValidator::MathBuiltin::Constant:
        return m.addMathBuiltinConstant(varName, mathBuiltin.u.cst, field);
      default:
        break;
    }
    MOZ_CRASH("unexpected or uninitialized math builtin type");
}

static bool
CheckGlobalAtomicsImport(ModuleValidator& m, ParseNode* initNode, PropertyName* varName,
                         PropertyName* field)
{
    // Atomics builtin, with the form glob.Atomics.[[builtin]]
    AsmJSAtomicsBuiltinFunction func;
    if (!m.lookupStandardLibraryAtomicsName(field, &func))
        return m.failName(initNode, "'%s' is not a standard Atomics builtin", field);

    return m.addAtomicsBuiltinFunction(varName, func, field);
}

static bool
CheckGlobalDotImport(ModuleValidator& m, PropertyName* varName, ParseNode* initNode)
{
    ParseNode* base = DotBase(initNode);
    PropertyName* field = DotMember(initNode);

    if (base->isKind(PNK_DOT)) {
        ParseNode* global = DotBase(base);
        PropertyName* mathOrAtomics = DotMember(base);

        PropertyName* globalName = m.globalArgumentName();
        if (!globalName)
            return m.fail(base, "import statement requires the module have a stdlib parameter");

        if (!IsUseOfName(global, globalName)) {
            if (global->isKind(PNK_DOT)) {
                return m.failName(base, "imports can have at most two dot accesses "
                                        "(e.g. %s.Math.sin)", globalName);
            }
            return m.failName(base, "expecting %s.*", globalName);
        }

        if (mathOrAtomics == m.cx()->names().Math)
            return CheckGlobalMathImport(m, initNode, varName, field);
        if (mathOrAtomics == m.cx()->names().Atomics)
            return CheckGlobalAtomicsImport(m, initNode, varName, field);
        return m.failName(base, "expecting %s.{Math|Atomics}", globalName);
    }

    if (!base->isKind(PNK_NAME))
        return m.fail(base, "expected name of variable or parameter");

    if (base->name() == m.globalArgumentName()) {
        if (field == m.cx()->names().NaN)
            return m.addGlobalConstant(varName, GenericNaN(), field);
        if (field == m.cx()->names().Infinity)
            return m.addGlobalConstant(varName, PositiveInfinity<double>(), field);

        Scalar::Type type;
        if (IsArrayViewCtorName(m, field, &type))
            return m.addArrayViewCtor(varName, type, field);

        return m.failName(initNode, "'%s' is not a standard constant or typed array name", field);
    }
    if (base->name() != m.importArgumentName())
        return m.fail(base, "expected global or import name");

    return m.addFFI(varName, field);
}

static bool
CheckModuleGlobal(ModuleValidator& m, ParseNode* var, bool isConst)
{
    if (!var->isKind(PNK_NAME))
        return m.fail(var, "import variable is not a plain name");

    if (!CheckModuleLevelName(m, var, var->name()))
        return false;

    ParseNode* initNode = MaybeInitializer(var);
    if (!initNode)
        return m.fail(var, "module import needs initializer");

    if (IsNumericLiteral(m, initNode))
        return CheckGlobalVariableInitConstant(m, var->name(), initNode, isConst);

    if (initNode->isKind(PNK_BITOR) || initNode->isKind(PNK_POS) || initNode->isKind(PNK_CALL))
        return CheckGlobalVariableInitImport(m, var->name(), initNode, isConst);

    if (initNode->isKind(PNK_NEW))
        return CheckNewArrayView(m, var->name(), initNode);

    if (initNode->isKind(PNK_DOT))
        return CheckGlobalDotImport(m, var->name(), initNode);

    return m.fail(initNode, "unsupported import expression");
}

static bool
CheckModuleProcessingDirectives(ModuleValidator& m)
{
    TokenStream& ts = m.parser().tokenStream;
    while (true) {
        bool matched;
        if (!ts.matchToken(&matched, TOK_STRING, TokenStream::Operand))
            return false;
        if (!matched)
            return true;

        if (!IsIgnoredDirectiveName(m.cx(), ts.currentToken().atom()))
            return m.failCurrentOffset("unsupported processing directive");

        TokenKind tt;
        if (!ts.getToken(&tt))
            return false;
        if (tt != TOK_SEMI)
            return m.failCurrentOffset("expected semicolon after string literal");
    }
}

static bool
CheckModuleGlobals(ModuleValidator& m)
{
    while (true) {
        ParseNode* varStmt;
        if (!ParseVarOrConstStatement(m.parser(), &varStmt))
            return false;
        if (!varStmt)
            break;
        for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
            if (!CheckModuleGlobal(m, var, varStmt->isKind(PNK_CONST)))
                return false;
        }
    }

    return true;
}

static bool
ArgFail(FunctionValidator& f, PropertyName* argName, ParseNode* stmt)
{
    return f.failName(stmt, "expecting argument type declaration for '%s' of the "
                      "form 'arg = arg|0' or 'arg = +arg' or 'arg = fround(arg)'", argName);
}

static bool
CheckArgumentType(FunctionValidator& f, ParseNode* stmt, PropertyName* name, Type* type)
{
    if (!stmt || !IsExpressionStatement(stmt))
        return ArgFail(f, name, stmt ? stmt : f.fn());

    ParseNode* initNode = ExpressionStatementExpr(stmt);
    if (!initNode || !initNode->isKind(PNK_ASSIGN))
        return ArgFail(f, name, stmt);

    ParseNode* argNode = BinaryLeft(initNode);
    ParseNode* coercionNode = BinaryRight(initNode);

    if (!IsUseOfName(argNode, name))
        return ArgFail(f, name, stmt);

    ParseNode* coercedExpr;
    if (!CheckTypeAnnotation(f.m(), coercionNode, type, &coercedExpr))
        return false;

    if (!type->isArgType())
        return f.failName(stmt, "invalid type for argument '%s'", name);

    if (!IsUseOfName(coercedExpr, name))
        return ArgFail(f, name, stmt);

    return true;
}

static bool
CheckProcessingDirectives(ModuleValidator& m, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    while (stmt && IsIgnoredDirective(m.cx(), stmt))
        stmt = NextNode(stmt);

    *stmtIter = stmt;
    return true;
}

static bool
CheckArguments(FunctionValidator& f, ParseNode** stmtIter, ValTypeVector* argTypes)
{
    ParseNode* stmt = *stmtIter;

    unsigned numFormals;
    ParseNode* argpn = FunctionFormalParametersList(f.fn(), &numFormals);

    for (unsigned i = 0; i < numFormals; i++, argpn = NextNode(argpn), stmt = NextNode(stmt)) {
        PropertyName* name;
        if (!CheckArgument(f.m(), argpn, &name))
            return false;

        Type type;
        if (!CheckArgumentType(f, stmt, name, &type))
            return false;

        if (!argTypes->append(type.canonicalToValType()))
            return false;

        if (!f.addLocal(argpn, name, type))
            return false;
    }

    *stmtIter = stmt;
    return true;
}

static bool
IsLiteralOrConst(FunctionValidator& f, ParseNode* pn, NumLit* lit)
{
    if (pn->isKind(PNK_NAME)) {
        const ModuleValidator::Global* global = f.lookupGlobal(pn->name());
        if (!global || global->which() != ModuleValidator::Global::ConstantLiteral)
            return false;

        *lit = global->constLiteralValue();
        return true;
    }

    if (!IsNumericLiteral(f.m(), pn))
        return false;

    *lit = ExtractNumericLiteral(f.m(), pn);
    return true;
}

static bool
CheckFinalReturn(FunctionValidator& f, ParseNode* lastNonEmptyStmt)
{
    if (!f.encoder().writeOp(Op::End))
        return false;

    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(ExprType::Void);
        return true;
    }

    if (!lastNonEmptyStmt->isKind(PNK_RETURN) && !IsVoid(f.returnedType()))
        return f.fail(lastNonEmptyStmt, "void incompatible with previous return type");

    return true;
}

static bool
CheckVariable(FunctionValidator& f, ParseNode* var, ValTypeVector* types, Vector<NumLit>* inits)
{
    if (!var->isKind(PNK_NAME))
        return f.fail(var, "local variable is not a plain name");

    PropertyName* name = var->name();

    if (!CheckIdentifier(f.m(), var, name))
        return false;

    ParseNode* initNode = MaybeInitializer(var);
    if (!initNode)
        return f.failName(var, "var '%s' needs explicit type declaration via an initial value", name);

    NumLit lit;
    if (!IsLiteralOrConst(f, initNode, &lit))
        return f.failName(var, "var '%s' initializer must be literal or const literal", name);

    if (!lit.valid())
        return f.failName(var, "var '%s' initializer out of range", name);

    Type type = Type::canonicalize(Type::lit(lit));

    return f.addLocal(var, name, type) &&
           types->append(type.canonicalToValType()) &&
           inits->append(lit);
}

static bool
CheckVariables(FunctionValidator& f, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    uint32_t firstVar = f.numLocals();

    ValTypeVector types;
    Vector<NumLit> inits(f.cx());

    for (; stmt && stmt->isKind(PNK_VAR); stmt = NextNonEmptyStatement(stmt)) {
        for (ParseNode* var = VarListHead(stmt); var; var = NextNode(var)) {
            if (!CheckVariable(f, var, &types, &inits))
                return false;
        }
    }

    MOZ_ASSERT(f.encoder().empty());

    if (!EncodeLocalEntries(f.encoder(), types))
        return false;

    for (uint32_t i = 0; i < inits.length(); i++) {
        NumLit lit = inits[i];
        if (lit.isZeroBits())
            continue;
        if (!f.writeConstExpr(lit))
            return false;
        if (!f.encoder().writeOp(Op::SetLocal))
            return false;
        if (!f.encoder().writeVarU32(firstVar + i))
            return false;
    }

    *stmtIter = stmt;
    return true;
}

static bool
CheckExpr(FunctionValidator& f, ParseNode* op, Type* type);

static bool
CheckNumericLiteral(FunctionValidator& f, ParseNode* num, Type* type)
{
    NumLit lit = ExtractNumericLiteral(f.m(), num);
    if (!lit.valid())
        return f.fail(num, "numeric literal out of representable integer range");
    *type = Type::lit(lit);
    return f.writeConstExpr(lit);
}

static bool
CheckVarRef(FunctionValidator& f, ParseNode* varRef, Type* type)
{
    PropertyName* name = varRef->name();

    if (const FunctionValidator::Local* local = f.lookupLocal(name)) {
        if (!f.encoder().writeOp(Op::GetLocal))
            return false;
        if (!f.encoder().writeVarU32(local->slot))
            return false;
        *type = local->type;
        return true;
    }

    if (const ModuleValidator::Global* global = f.lookupGlobal(name)) {
        switch (global->which()) {
          case ModuleValidator::Global::ConstantLiteral:
            *type = global->varOrConstType();
            return f.writeConstExpr(global->constLiteralValue());
          case ModuleValidator::Global::ConstantImport:
          case ModuleValidator::Global::Variable: {
            *type = global->varOrConstType();
            return f.encoder().writeOp(Op::GetGlobal) &&
                   f.encoder().writeVarU32(global->varOrConstIndex());
          }
          case ModuleValidator::Global::Function:
          case ModuleValidator::Global::FFI:
          case ModuleValidator::Global::MathBuiltinFunction:
          case ModuleValidator::Global::AtomicsBuiltinFunction:
          case ModuleValidator::Global::FuncPtrTable:
          case ModuleValidator::Global::ArrayView:
          case ModuleValidator::Global::ArrayViewCtor:
            break;
        }
        return f.failName(varRef, "'%s' may not be accessed by ordinary expressions", name);
    }

    return f.failName(varRef, "'%s' not found in local or asm.js module scope", name);
}

static inline bool
IsLiteralOrConstInt(FunctionValidator& f, ParseNode* pn, uint32_t* u32)
{
    NumLit lit;
    if (!IsLiteralOrConst(f, pn, &lit))
        return false;

    return IsLiteralInt(lit, u32);
}

static const int32_t NoMask = -1;

static bool
CheckArrayAccess(FunctionValidator& f, ParseNode* viewName, ParseNode* indexExpr,
                 Scalar::Type* viewType)
{
    if (!viewName->isKind(PNK_NAME))
        return f.fail(viewName, "base of array access must be a typed array view name");

    const ModuleValidator::Global* global = f.lookupGlobal(viewName->name());
    if (!global || !global->isAnyArrayView())
        return f.fail(viewName, "base of array access must be a typed array view name");

    *viewType = global->viewType();

    uint32_t index;
    if (IsLiteralOrConstInt(f, indexExpr, &index)) {
        uint64_t byteOffset = uint64_t(index) << TypedArrayShift(*viewType);
        uint64_t width = TypedArrayElemSize(*viewType);
        if (!f.m().tryConstantAccess(byteOffset, width))
            return f.fail(indexExpr, "constant index out of range");

        return f.writeInt32Lit(byteOffset);
    }

    // Mask off the low bits to account for the clearing effect of a right shift
    // followed by the left shift implicit in the array access. E.g., H32[i>>2]
    // loses the low two bits.
    int32_t mask = ~(TypedArrayElemSize(*viewType) - 1);

    if (indexExpr->isKind(PNK_RSH)) {
        ParseNode* shiftAmountNode = BitwiseRight(indexExpr);

        uint32_t shift;
        if (!IsLiteralInt(f.m(), shiftAmountNode, &shift))
            return f.failf(shiftAmountNode, "shift amount must be constant");

        unsigned requiredShift = TypedArrayShift(*viewType);
        if (shift != requiredShift)
            return f.failf(shiftAmountNode, "shift amount must be %u", requiredShift);

        ParseNode* pointerNode = BitwiseLeft(indexExpr);

        Type pointerType;
        if (!CheckExpr(f, pointerNode, &pointerType))
            return false;

        if (!pointerType.isIntish())
            return f.failf(pointerNode, "%s is not a subtype of int", pointerType.toChars());
    } else {
        // For legacy scalar access compatibility, accept Int8/Uint8 accesses with no shift.
        if (TypedArrayShift(*viewType) != 0)
            return f.fail(indexExpr, "index expression isn't shifted; must be an Int8/Uint8 access");

        MOZ_ASSERT(mask == NoMask);

        ParseNode* pointerNode = indexExpr;

        Type pointerType;
        if (!CheckExpr(f, pointerNode, &pointerType))
            return false;

        if (!pointerType.isInt())
            return f.failf(pointerNode, "%s is not a subtype of int", pointerType.toChars());
    }

    // Don't generate the mask op if there is no need for it which could happen
    // for a shift of zero.
    if (mask != NoMask) {
        return f.writeInt32Lit(mask) &&
               f.encoder().writeOp(Op::I32And);
    }

    return true;
}

static bool
WriteArrayAccessFlags(FunctionValidator& f, Scalar::Type viewType)
{
    // asm.js only has naturally-aligned accesses.
    size_t align = TypedArrayElemSize(viewType);
    MOZ_ASSERT(IsPowerOfTwo(align));
    if (!f.encoder().writeFixedU8(CeilingLog2(align)))
        return false;

    // asm.js doesn't have constant offsets, so just encode a 0.
    if (!f.encoder().writeVarU32(0))
        return false;

    return true;
}

static bool
CheckLoadArray(FunctionValidator& f, ParseNode* elem, Type* type)
{
    Scalar::Type viewType;

    if (!CheckArrayAccess(f, ElemBase(elem), ElemIndex(elem), &viewType))
        return false;

    switch (viewType) {
      case Scalar::Int8:    if (!f.encoder().writeOp(Op::I32Load8S))  return false; break;
      case Scalar::Uint8:   if (!f.encoder().writeOp(Op::I32Load8U))  return false; break;
      case Scalar::Int16:   if (!f.encoder().writeOp(Op::I32Load16S)) return false; break;
      case Scalar::Uint16:  if (!f.encoder().writeOp(Op::I32Load16U)) return false; break;
      case Scalar::Uint32:
      case Scalar::Int32:   if (!f.encoder().writeOp(Op::I32Load))    return false; break;
      case Scalar::Float32: if (!f.encoder().writeOp(Op::F32Load))    return false; break;
      case Scalar::Float64: if (!f.encoder().writeOp(Op::F64Load))    return false; break;
      default: MOZ_CRASH("unexpected scalar type");
    }

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        *type = Type::Intish;
        break;
      case Scalar::Float32:
        *type = Type::MaybeFloat;
        break;
      case Scalar::Float64:
        *type = Type::MaybeDouble;
        break;
      default: MOZ_CRASH("Unexpected array type");
    }

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    return true;
}

static bool
CheckStoreArray(FunctionValidator& f, ParseNode* lhs, ParseNode* rhs, Type* type)
{
    Scalar::Type viewType;
    if (!CheckArrayAccess(f, ElemBase(lhs), ElemIndex(lhs), &viewType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        if (!rhsType.isIntish())
            return f.failf(lhs, "%s is not a subtype of intish", rhsType.toChars());
        break;
      case Scalar::Float32:
        if (!rhsType.isMaybeDouble() && !rhsType.isFloatish())
            return f.failf(lhs, "%s is not a subtype of double? or floatish", rhsType.toChars());
        break;
      case Scalar::Float64:
        if (!rhsType.isMaybeFloat() && !rhsType.isMaybeDouble())
            return f.failf(lhs, "%s is not a subtype of float? or double?", rhsType.toChars());
        break;
      default:
        MOZ_CRASH("Unexpected view type");
    }

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        if (!f.encoder().writeOp(Op::I32TeeStore8))
            return false;
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        if (!f.encoder().writeOp(Op::I32TeeStore16))
            return false;
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        if (!f.encoder().writeOp(Op::I32TeeStore))
            return false;
        break;
      case Scalar::Float32:
        if (rhsType.isFloatish()) {
            if (!f.encoder().writeOp(Op::F32TeeStore))
                return false;
        } else {
            if (!f.encoder().writeOp(Op::F64TeeStoreF32))
                return false;
        }
        break;
      case Scalar::Float64:
        if (rhsType.isFloatish()) {
            if (!f.encoder().writeOp(Op::F32TeeStoreF64))
                return false;
        } else {
            if (!f.encoder().writeOp(Op::F64TeeStore))
                return false;
        }
        break;
      default: MOZ_CRASH("unexpected scalar type");
    }

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = rhsType;
    return true;
}

static bool
CheckAssignName(FunctionValidator& f, ParseNode* lhs, ParseNode* rhs, Type* type)
{
    RootedPropertyName name(f.cx(), lhs->name());

    if (const FunctionValidator::Local* lhsVar = f.lookupLocal(name)) {
        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;

        if (!f.encoder().writeOp(Op::TeeLocal))
            return false;
        if (!f.encoder().writeVarU32(lhsVar->slot))
            return false;

        if (!(rhsType <= lhsVar->type)) {
            return f.failf(lhs, "%s is not a subtype of %s",
                           rhsType.toChars(), lhsVar->type.toChars());
        }
        *type = rhsType;
        return true;
    }

    if (const ModuleValidator::Global* global = f.lookupGlobal(name)) {
        if (global->which() != ModuleValidator::Global::Variable)
            return f.failName(lhs, "'%s' is not a mutable variable", name);

        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;

        Type globType = global->varOrConstType();
        if (!(rhsType <= globType))
            return f.failf(lhs, "%s is not a subtype of %s", rhsType.toChars(), globType.toChars());
        if (!f.encoder().writeOp(Op::TeeGlobal))
            return false;
        if (!f.encoder().writeVarU32(global->varOrConstIndex()))
            return false;

        *type = rhsType;
        return true;
    }

    return f.failName(lhs, "'%s' not found in local or asm.js module scope", name);
}

static bool
CheckAssign(FunctionValidator& f, ParseNode* assign, Type* type)
{
    MOZ_ASSERT(assign->isKind(PNK_ASSIGN));

    ParseNode* lhs = BinaryLeft(assign);
    ParseNode* rhs = BinaryRight(assign);

    if (lhs->getKind() == PNK_ELEM)
        return CheckStoreArray(f, lhs, rhs, type);

    if (lhs->getKind() == PNK_NAME)
        return CheckAssignName(f, lhs, rhs, type);

    return f.fail(assign, "left-hand side of assignment must be a variable or array access");
}

static bool
CheckMathIMul(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Math.imul must be passed 2 arguments");

    ParseNode* lhs = CallArgList(call);
    ParseNode* rhs = NextNode(lhs);

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    *type = Type::Signed;
    return f.encoder().writeOp(Op::I32Mul);
}

static bool
CheckMathClz32(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.clz32 must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (!argType.isIntish())
        return f.failf(arg, "%s is not a subtype of intish", argType.toChars());

    *type = Type::Fixnum;
    return f.encoder().writeOp(Op::I32Clz);
}

static bool
CheckMathAbs(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.abs must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (argType.isSigned()) {
        *type = Type::Unsigned;
        return f.encoder().writeOp(Op::I32Abs);
    }

    if (argType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Abs);
    }

    if (argType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Abs);
    }

    return f.failf(call, "%s is not a subtype of signed, float? or double?", argType.toChars());
}

static bool
CheckMathSqrt(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.sqrt must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (argType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Sqrt);
    }

    if (argType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Sqrt);
    }

    return f.failf(call, "%s is neither a subtype of double? nor float?", argType.toChars());
}

static bool
CheckMathMinMax(FunctionValidator& f, ParseNode* callNode, bool isMax, Type* type)
{
    if (CallArgListLength(callNode) < 2)
        return f.fail(callNode, "Math.min/max must be passed at least 2 arguments");

    ParseNode* firstArg = CallArgList(callNode);
    Type firstType;
    if (!CheckExpr(f, firstArg, &firstType))
        return false;

    Op op;
    if (firstType.isMaybeDouble()) {
        *type = Type::Double;
        firstType = Type::MaybeDouble;
        op = isMax ? Op::F64Max : Op::F64Min;
    } else if (firstType.isMaybeFloat()) {
        *type = Type::Float;
        firstType = Type::MaybeFloat;
        op = isMax ? Op::F32Max : Op::F32Min;
    } else if (firstType.isSigned()) {
        *type = Type::Signed;
        firstType = Type::Signed;
        op = isMax ? Op::I32Max : Op::I32Min;
    } else {
        return f.failf(firstArg, "%s is not a subtype of double?, float? or signed",
                       firstType.toChars());
    }

    unsigned numArgs = CallArgListLength(callNode);
    ParseNode* nextArg = NextNode(firstArg);
    for (unsigned i = 1; i < numArgs; i++, nextArg = NextNode(nextArg)) {
        Type nextType;
        if (!CheckExpr(f, nextArg, &nextType))
            return false;
        if (!(nextType <= firstType))
            return f.failf(nextArg, "%s is not a subtype of %s", nextType.toChars(), firstType.toChars());

        if (!f.encoder().writeOp(op))
            return false;
    }

    return true;
}

static bool
CheckSharedArrayAtomicAccess(FunctionValidator& f, ParseNode* viewName, ParseNode* indexExpr,
                             Scalar::Type* viewType)
{
    if (!CheckArrayAccess(f, viewName, indexExpr, viewType))
        return false;

    // The global will be sane, CheckArrayAccess checks it.
    const ModuleValidator::Global* global = f.lookupGlobal(viewName->name());
    if (global->which() != ModuleValidator::Global::ArrayView)
        return f.fail(viewName, "base of array access must be a typed array view");

    MOZ_ASSERT(f.m().atomicsPresent());

    switch (*viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        return true;
      default:
        return f.failf(viewName, "not an integer array");
    }

    return true;
}

static bool
WriteAtomicOperator(FunctionValidator& f, Op opcode, Scalar::Type viewType)
{
    return f.encoder().writeOp(opcode) &&
           f.encoder().writeFixedU8(viewType);
}

static bool
CheckAtomicsLoad(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Atomics.load must be passed 2 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, Op::I32AtomicsLoad, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsStore(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics.store must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type rhsType;
    if (!CheckExpr(f, valueArg, &rhsType))
        return false;

    if (!rhsType.isIntish())
        return f.failf(arrayArg, "%s is not a subtype of intish", rhsType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, Op::I32AtomicsStore, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = rhsType;
    return true;
}

static bool
CheckAtomicsBinop(FunctionValidator& f, ParseNode* call, Type* type, AtomicOp op)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics binary operator must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type valueArgType;
    if (!CheckExpr(f, valueArg, &valueArgType))
        return false;

    if (!valueArgType.isIntish())
        return f.failf(valueArg, "%s is not a subtype of intish", valueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, Op::I32AtomicsBinOp, viewType))
        return false;
    if (!f.encoder().writeFixedU8(uint8_t(op)))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsIsLockFree(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Atomics.isLockFree must be passed 1 argument");

    ParseNode* sizeArg = CallArgList(call);

    uint32_t size;
    if (!IsLiteralInt(f.m(), sizeArg, &size))
        return f.fail(sizeArg, "Atomics.isLockFree requires an integer literal argument");

    *type = Type::Int;
    return f.writeInt32Lit(AtomicOperations::isLockfree(size));
}

static bool
CheckAtomicsCompareExchange(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 4)
        return f.fail(call, "Atomics.compareExchange must be passed 4 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* oldValueArg = NextNode(indexArg);
    ParseNode* newValueArg = NextNode(oldValueArg);

    Type oldValueArgType;
    if (!CheckExpr(f, oldValueArg, &oldValueArgType))
        return false;

    Type newValueArgType;
    if (!CheckExpr(f, newValueArg, &newValueArgType))
        return false;

    if (!oldValueArgType.isIntish())
        return f.failf(oldValueArg, "%s is not a subtype of intish", oldValueArgType.toChars());

    if (!newValueArgType.isIntish())
        return f.failf(newValueArg, "%s is not a subtype of intish", newValueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, Op::I32AtomicsCompareExchange, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsExchange(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics.exchange must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type valueArgType;
    if (!CheckExpr(f, valueArg, &valueArgType))
        return false;

    if (!valueArgType.isIntish())
        return f.failf(arrayArg, "%s is not a subtype of intish", valueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, Op::I32AtomicsExchange, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSAtomicsBuiltinFunction func,
                        Type* type)
{
    f.setUsesAtomics();

    switch (func) {
      case AsmJSAtomicsBuiltin_compareExchange:
        return CheckAtomicsCompareExchange(f, callNode, type);
      case AsmJSAtomicsBuiltin_exchange:
        return CheckAtomicsExchange(f, callNode, type);
      case AsmJSAtomicsBuiltin_load:
        return CheckAtomicsLoad(f, callNode, type);
      case AsmJSAtomicsBuiltin_store:
        return CheckAtomicsStore(f, callNode, type);
      case AsmJSAtomicsBuiltin_add:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchAddOp);
      case AsmJSAtomicsBuiltin_sub:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchSubOp);
      case AsmJSAtomicsBuiltin_and:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchAndOp);
      case AsmJSAtomicsBuiltin_or:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchOrOp);
      case AsmJSAtomicsBuiltin_xor:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchXorOp);
      case AsmJSAtomicsBuiltin_isLockFree:
        return CheckAtomicsIsLockFree(f, callNode, type);
      default:
        MOZ_CRASH("unexpected atomicsBuiltin function");
    }
}

typedef bool (*CheckArgType)(FunctionValidator& f, ParseNode* argNode, Type type);

template <CheckArgType checkArg>
static bool
CheckCallArgs(FunctionValidator& f, ParseNode* callNode, ValTypeVector* args)
{
    ParseNode* argNode = CallArgList(callNode);
    for (unsigned i = 0; i < CallArgListLength(callNode); i++, argNode = NextNode(argNode)) {
        Type type;
        if (!CheckExpr(f, argNode, &type))
            return false;

        if (!checkArg(f, argNode, type))
            return false;

        if (!args->append(Type::canonicalize(type).canonicalToValType()))
            return false;
    }
    return true;
}

static bool
CheckSignatureAgainstExisting(ModuleValidator& m, ParseNode* usepn, const Sig& sig, const Sig& existing)
{
    if (sig.args().length() != existing.args().length()) {
        return m.failf(usepn, "incompatible number of arguments (%" PRIuSIZE
                       " here vs. %" PRIuSIZE " before)",
                       sig.args().length(), existing.args().length());
    }

    for (unsigned i = 0; i < sig.args().length(); i++) {
        if (sig.arg(i) != existing.arg(i)) {
            return m.failf(usepn, "incompatible type for argument %u: (%s here vs. %s before)", i,
                           ToCString(sig.arg(i)), ToCString(existing.arg(i)));
        }
    }

    if (sig.ret() != existing.ret()) {
        return m.failf(usepn, "%s incompatible with previous return of type %s",
                       ToCString(sig.ret()), ToCString(existing.ret()));
    }

    MOZ_ASSERT(sig == existing);
    return true;
}

static bool
CheckFunctionSignature(ModuleValidator& m, ParseNode* usepn, Sig&& sig, PropertyName* name,
                       ModuleValidator::Func** func)
{
    ModuleValidator::Func* existing = m.lookupFunction(name);
    if (!existing) {
        if (!CheckModuleLevelName(m, usepn, name))
            return false;
        return m.addFunction(name, usepn->pn_pos.begin, Move(sig), func);
    }

    if (!CheckSignatureAgainstExisting(m, usepn, sig, m.mg().funcSig(existing->index())))
        return false;

    *func = existing;
    return true;
}

static bool
CheckIsArgType(FunctionValidator& f, ParseNode* argNode, Type type)
{
    if (!type.isArgType())
        return f.failf(argNode, "%s is not a subtype of int, float or double", type.toChars());

    return true;
}

static bool
CheckInternalCall(FunctionValidator& f, ParseNode* callNode, PropertyName* calleeName,
                  Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    ModuleValidator::Func* callee;
    if (!CheckFunctionSignature(f.m(), callNode, Move(sig), calleeName, &callee))
        return false;

    if (!f.writeCall(callNode, Op::Call))
        return false;

    if (!f.encoder().writeVarU32(callee->index()))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckFuncPtrTableAgainstExisting(ModuleValidator& m, ParseNode* usepn, PropertyName* name,
                                 Sig&& sig, unsigned mask, uint32_t* funcPtrTableIndex)
{
    if (const ModuleValidator::Global* existing = m.lookupGlobal(name)) {
        if (existing->which() != ModuleValidator::Global::FuncPtrTable)
            return m.failName(usepn, "'%s' is not a function-pointer table", name);

        ModuleValidator::FuncPtrTable& table = m.funcPtrTable(existing->funcPtrTableIndex());
        if (mask != table.mask())
            return m.failf(usepn, "mask does not match previous value (%u)", table.mask());

        if (!CheckSignatureAgainstExisting(m, usepn, sig, m.mg().sig(table.sigIndex())))
            return false;

        *funcPtrTableIndex = existing->funcPtrTableIndex();
        return true;
    }

    if (!CheckModuleLevelName(m, usepn, name))
        return false;

    if (!m.declareFuncPtrTable(Move(sig), name, usepn->pn_pos.begin, mask, funcPtrTableIndex))
        return false;

    return true;
}

static bool
CheckFuncPtrCall(FunctionValidator& f, ParseNode* callNode, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    ParseNode* callee = CallCallee(callNode);
    ParseNode* tableNode = ElemBase(callee);
    ParseNode* indexExpr = ElemIndex(callee);

    if (!tableNode->isKind(PNK_NAME))
        return f.fail(tableNode, "expecting name of function-pointer array");

    PropertyName* name = tableNode->name();
    if (const ModuleValidator::Global* existing = f.lookupGlobal(name)) {
        if (existing->which() != ModuleValidator::Global::FuncPtrTable)
            return f.failName(tableNode, "'%s' is not the name of a function-pointer array", name);
    }

    if (!indexExpr->isKind(PNK_BITAND))
        return f.fail(indexExpr, "function-pointer table index expression needs & mask");

    ParseNode* indexNode = BitwiseLeft(indexExpr);
    ParseNode* maskNode = BitwiseRight(indexExpr);

    uint32_t mask;
    if (!IsLiteralInt(f.m(), maskNode, &mask) || mask == UINT32_MAX || !IsPowerOfTwo(mask + 1))
        return f.fail(maskNode, "function-pointer table index mask value must be a power of two minus 1");

    Type indexType;
    if (!CheckExpr(f, indexNode, &indexType))
        return false;

    if (!indexType.isIntish())
        return f.failf(indexNode, "%s is not a subtype of intish", indexType.toChars());

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    uint32_t tableIndex;
    if (!CheckFuncPtrTableAgainstExisting(f.m(), tableNode, name, Move(sig), mask, &tableIndex))
        return false;

    if (!f.writeCall(callNode, Op::OldCallIndirect))
        return false;

    // Call signature
    if (!f.encoder().writeVarU32(f.m().funcPtrTable(tableIndex).sigIndex()))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckIsExternType(FunctionValidator& f, ParseNode* argNode, Type type)
{
    if (!type.isExtern())
        return f.failf(argNode, "%s is not a subtype of extern", type.toChars());
    return true;
}

static bool
CheckFFICall(FunctionValidator& f, ParseNode* callNode, unsigned ffiIndex, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    PropertyName* calleeName = CallCallee(callNode)->name();

    if (ret.isFloat())
        return f.fail(callNode, "FFI calls can't return float");

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsExternType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    uint32_t funcIndex;
    if (!f.m().declareImport(calleeName, Move(sig), ffiIndex, &funcIndex))
        return false;

    if (!f.writeCall(callNode, Op::Call))
        return false;

    if (!f.encoder().writeVarU32(funcIndex))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckFloatCoercionArg(FunctionValidator& f, ParseNode* inputNode, Type inputType)
{
    if (inputType.isMaybeDouble())
        return f.encoder().writeOp(Op::F32DemoteF64);
    if (inputType.isSigned())
        return f.encoder().writeOp(Op::F32ConvertSI32);
    if (inputType.isUnsigned())
        return f.encoder().writeOp(Op::F32ConvertUI32);
    if (inputType.isFloatish())
        return true;

    return f.failf(inputNode, "%s is not a subtype of signed, unsigned, double? or floatish",
                   inputType.toChars());
}

static bool
CheckCoercedCall(FunctionValidator& f, ParseNode* call, Type ret, Type* type);

static bool
CheckCoercionArg(FunctionValidator& f, ParseNode* arg, Type expected, Type* type)
{
    MOZ_ASSERT(expected.isCanonicalValType());

    if (arg->isKind(PNK_CALL))
        return CheckCoercedCall(f, arg, expected, type);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (expected.isFloat()) {
        if (!CheckFloatCoercionArg(f, arg, argType))
            return false;
    } else {
        MOZ_CRASH("not call coercions");
    }

    *type = Type::ret(expected);
    return true;
}

static bool
CheckMathFRound(FunctionValidator& f, ParseNode* callNode, Type* type)
{
    if (CallArgListLength(callNode) != 1)
        return f.fail(callNode, "Math.fround must be passed 1 argument");

    ParseNode* argNode = CallArgList(callNode);
    Type argType;
    if (!CheckCoercionArg(f, argNode, Type::Float, &argType))
        return false;

    MOZ_ASSERT(argType == Type::Float);
    *type = Type::Float;
    return true;
}

static bool
CheckMathBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                     Type* type)
{
    unsigned arity = 0;
    Op f32;
    Op f64;
    switch (func) {
      case AsmJSMathBuiltin_imul:   return CheckMathIMul(f, callNode, type);
      case AsmJSMathBuiltin_clz32:  return CheckMathClz32(f, callNode, type);
      case AsmJSMathBuiltin_abs:    return CheckMathAbs(f, callNode, type);
      case AsmJSMathBuiltin_sqrt:   return CheckMathSqrt(f, callNode, type);
      case AsmJSMathBuiltin_fround: return CheckMathFRound(f, callNode, type);
      case AsmJSMathBuiltin_min:    return CheckMathMinMax(f, callNode, /* isMax = */ false, type);
      case AsmJSMathBuiltin_max:    return CheckMathMinMax(f, callNode, /* isMax = */ true, type);
      case AsmJSMathBuiltin_ceil:   arity = 1; f64 = Op::F64Ceil;  f32 = Op::F32Ceil;     break;
      case AsmJSMathBuiltin_floor:  arity = 1; f64 = Op::F64Floor; f32 = Op::F32Floor;    break;
      case AsmJSMathBuiltin_sin:    arity = 1; f64 = Op::F64Sin;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_cos:    arity = 1; f64 = Op::F64Cos;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_tan:    arity = 1; f64 = Op::F64Tan;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_asin:   arity = 1; f64 = Op::F64Asin;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_acos:   arity = 1; f64 = Op::F64Acos;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_atan:   arity = 1; f64 = Op::F64Atan;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_exp:    arity = 1; f64 = Op::F64Exp;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_log:    arity = 1; f64 = Op::F64Log;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_pow:    arity = 2; f64 = Op::F64Pow;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_atan2:  arity = 2; f64 = Op::F64Atan2; f32 = Op::Unreachable; break;
      default: MOZ_CRASH("unexpected mathBuiltin function");
    }

    unsigned actualArity = CallArgListLength(callNode);
    if (actualArity != arity)
        return f.failf(callNode, "call passed %u arguments, expected %u", actualArity, arity);

    if (!f.prepareCall(callNode))
        return false;

    Type firstType;
    ParseNode* argNode = CallArgList(callNode);
    if (!CheckExpr(f, argNode, &firstType))
        return false;

    if (!firstType.isMaybeFloat() && !firstType.isMaybeDouble())
        return f.fail(argNode, "arguments to math call should be a subtype of double? or float?");

    bool opIsDouble = firstType.isMaybeDouble();
    if (!opIsDouble && f32 == Op::Unreachable)
        return f.fail(callNode, "math builtin cannot be used as float");

    if (arity == 2) {
        Type secondType;
        argNode = NextNode(argNode);
        if (!CheckExpr(f, argNode, &secondType))
            return false;

        if (firstType.isMaybeDouble() && !secondType.isMaybeDouble())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");
        if (firstType.isMaybeFloat() && !secondType.isMaybeFloat())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");
    }

    if (opIsDouble) {
        if (!f.encoder().writeOp(f64))
            return false;
    } else {
        if (!f.encoder().writeOp(f32))
            return false;
    }

    *type = opIsDouble ? Type::Double : Type::Floatish;
    return true;
}

static bool
CheckUncoercedCall(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_CALL));

    const ModuleValidator::Global* global;
    if (IsCallToGlobal(f.m(), expr, &global)) {
        if (global->isMathFunction())
            return CheckMathBuiltinCall(f, expr, global->mathBuiltinFunction(), type);
        if (global->isAtomicsFunction())
            return CheckAtomicsBuiltinCall(f, expr, global->atomicsBuiltinFunction(), type);
    }

    return f.fail(expr, "all function calls must either be calls to standard lib math functions, "
                        "standard atomic functions ignored (via f(); or comma-expression), coerced"
                        " to signed (via f()|0), coerced to float (via fround(f())) or coerced to "
                        "double (via +f())");
}

static bool
CoerceResult(FunctionValidator& f, ParseNode* expr, Type expected, Type actual,
             Type* type)
{
    MOZ_ASSERT(expected.isCanonical());

    // At this point, the bytecode resembles this:
    //      | the thing we wanted to coerce | current position |>
    switch (expected.which()) {
      case Type::Void:
        if (!actual.isVoid()) {
            if (!f.encoder().writeOp(Op::Drop))
                return false;
        }
        break;
      case Type::Int:
        if (!actual.isIntish())
            return f.failf(expr, "%s is not a subtype of intish", actual.toChars());
        break;
      case Type::Float:
        if (!CheckFloatCoercionArg(f, expr, actual))
            return false;
        break;
      case Type::Double:
        if (actual.isMaybeDouble()) {
            // No conversion necessary.
        } else if (actual.isMaybeFloat()) {
            if (!f.encoder().writeOp(Op::F64PromoteF32))
                return false;
        } else if (actual.isSigned()) {
            if (!f.encoder().writeOp(Op::F64ConvertSI32))
                return false;
        } else if (actual.isUnsigned()) {
            if (!f.encoder().writeOp(Op::F64ConvertUI32))
                return false;
        } else {
            return f.failf(expr, "%s is not a subtype of double?, float?, signed or unsigned", actual.toChars());
        }
        break;
      default:
        MOZ_CRASH("unexpected uncoerced result type");
    }

    *type = Type::ret(expected);
    return true;
}

static bool
CheckCoercedMathBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                            Type ret, Type* type)
{
    Type actual;
    if (!CheckMathBuiltinCall(f, callNode, func, &actual))
        return false;
    return CoerceResult(f, callNode, ret, actual, type);
}

static bool
CheckCoercedAtomicsBuiltinCall(FunctionValidator& f, ParseNode* callNode,
                               AsmJSAtomicsBuiltinFunction func, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    Type actual;
    if (!CheckAtomicsBuiltinCall(f, callNode, func, &actual))
        return false;
    return CoerceResult(f, callNode, ret, actual, type);
}

static bool
CheckCoercedCall(FunctionValidator& f, ParseNode* call, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (IsNumericLiteral(f.m(), call)) {
        NumLit lit = ExtractNumericLiteral(f.m(), call);
        if (!f.writeConstExpr(lit))
            return false;
        return CoerceResult(f, call, ret, Type::lit(lit), type);
    }

    ParseNode* callee = CallCallee(call);

    if (callee->isKind(PNK_ELEM))
        return CheckFuncPtrCall(f, call, ret, type);

    if (!callee->isKind(PNK_NAME))
        return f.fail(callee, "unexpected callee expression type");

    PropertyName* calleeName = callee->name();

    if (const ModuleValidator::Global* global = f.lookupGlobal(calleeName)) {
        switch (global->which()) {
          case ModuleValidator::Global::FFI:
            return CheckFFICall(f, call, global->ffiIndex(), ret, type);
          case ModuleValidator::Global::MathBuiltinFunction:
            return CheckCoercedMathBuiltinCall(f, call, global->mathBuiltinFunction(), ret, type);
          case ModuleValidator::Global::AtomicsBuiltinFunction:
            return CheckCoercedAtomicsBuiltinCall(f, call, global->atomicsBuiltinFunction(), ret, type);
          case ModuleValidator::Global::ConstantLiteral:
          case ModuleValidator::Global::ConstantImport:
          case ModuleValidator::Global::Variable:
          case ModuleValidator::Global::FuncPtrTable:
          case ModuleValidator::Global::ArrayView:
          case ModuleValidator::Global::ArrayViewCtor:
            return f.failName(callee, "'%s' is not callable function", callee->name());
          case ModuleValidator::Global::Function:
            break;
        }
    }

    return CheckInternalCall(f, call, calleeName, ret, type);
}

static bool
CheckPos(FunctionValidator& f, ParseNode* pos, Type* type)
{
    MOZ_ASSERT(pos->isKind(PNK_POS));
    ParseNode* operand = UnaryKid(pos);

    if (operand->isKind(PNK_CALL))
        return CheckCoercedCall(f, operand, Type::Double, type);

    Type actual;
    if (!CheckExpr(f, operand, &actual))
        return false;

    return CoerceResult(f, operand, Type::Double, actual, type);
}

static bool
CheckNot(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_NOT));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (!operandType.isInt())
        return f.failf(operand, "%s is not a subtype of int", operandType.toChars());

    *type = Type::Int;
    return f.encoder().writeOp(Op::I32Eqz);
}

static bool
CheckNeg(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_NEG));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (operandType.isInt()) {
        *type = Type::Intish;
        return f.encoder().writeOp(Op::I32Neg);
    }

    if (operandType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Neg);
    }

    if (operandType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Neg);
    }

    return f.failf(operand, "%s is not a subtype of int, float? or double?", operandType.toChars());
}

static bool
CheckCoerceToInt(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_BITNOT));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (operandType.isMaybeDouble() || operandType.isMaybeFloat()) {
        *type = Type::Signed;
        Op opcode = operandType.isMaybeDouble() ? Op::I32TruncSF64 : Op::I32TruncSF32;
        return f.encoder().writeOp(opcode);
    }

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of double?, float? or intish", operandType.toChars());

    *type = Type::Signed;
    return true;
}

static bool
CheckBitNot(FunctionValidator& f, ParseNode* neg, Type* type)
{
    MOZ_ASSERT(neg->isKind(PNK_BITNOT));
    ParseNode* operand = UnaryKid(neg);

    if (operand->isKind(PNK_BITNOT))
        return CheckCoerceToInt(f, operand, type);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of intish", operandType.toChars());

    if (!f.encoder().writeOp(Op::I32BitNot))
        return false;

    *type = Type::Signed;
    return true;
}

static bool
CheckAsExprStatement(FunctionValidator& f, ParseNode* exprStmt);

static bool
CheckComma(FunctionValidator& f, ParseNode* comma, Type* type)
{
    MOZ_ASSERT(comma->isKind(PNK_COMMA));
    ParseNode* operands = ListHead(comma);

    // The block depth isn't taken into account here, because a comma list can't
    // contain breaks and continues and nested control flow structures.
    if (!f.encoder().writeOp(Op::Block))
        return false;

    size_t typeAt;
    if (!f.encoder().writePatchableFixedU7(&typeAt))
        return false;

    ParseNode* pn = operands;
    for (; NextNode(pn); pn = NextNode(pn)) {
        if (!CheckAsExprStatement(f, pn))
            return false;
    }

    if (!CheckExpr(f, pn, type))
        return false;

    f.encoder().patchFixedU7(typeAt, uint8_t(type->toWasmBlockSignatureType()));

    return f.encoder().writeOp(Op::End);
}

static bool
CheckConditional(FunctionValidator& f, ParseNode* ternary, Type* type)
{
    MOZ_ASSERT(ternary->isKind(PNK_CONDITIONAL));

    ParseNode* cond = TernaryKid1(ternary);
    ParseNode* thenExpr = TernaryKid2(ternary);
    ParseNode* elseExpr = TernaryKid3(ternary);

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;

    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    size_t typeAt;
    if (!f.pushIf(&typeAt))
        return false;

    Type thenType;
    if (!CheckExpr(f, thenExpr, &thenType))
        return false;

    if (!f.switchToElse())
        return false;

    Type elseType;
    if (!CheckExpr(f, elseExpr, &elseType))
        return false;

    if (thenType.isInt() && elseType.isInt()) {
        *type = Type::Int;
    } else if (thenType.isDouble() && elseType.isDouble()) {
        *type = Type::Double;
    } else if (thenType.isFloat() && elseType.isFloat()) {
        *type = Type::Float;
    } else {
        return f.failf(ternary, "then/else branches of conditional must both produce int, float, "
                       "or double; current types are %s and %s",
                       thenType.toChars(), elseType.toChars());
    }

    if (!f.popIf(typeAt, type->toWasmBlockSignatureType()))
        return false;

    return true;
}

static bool
IsValidIntMultiplyConstant(ModuleValidator& m, ParseNode* expr)
{
    if (!IsNumericLiteral(m, expr))
        return false;

    NumLit lit = ExtractNumericLiteral(m, expr);
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::NegativeInt:
        if (abs(lit.toInt32()) < (1<<20))
            return true;
        return false;
      case NumLit::BigUnsigned:
      case NumLit::Double:
      case NumLit::Float:
      case NumLit::OutOfRangeInt:
        return false;
    }

    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal");
}

static bool
CheckMultiply(FunctionValidator& f, ParseNode* star, Type* type)
{
    MOZ_ASSERT(star->isKind(PNK_STAR));
    ParseNode* lhs = MultiplyLeft(star);
    ParseNode* rhs = MultiplyRight(star);

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (lhsType.isInt() && rhsType.isInt()) {
        if (!IsValidIntMultiplyConstant(f.m(), lhs) && !IsValidIntMultiplyConstant(f.m(), rhs))
            return f.fail(star, "one arg to int multiply must be a small (-2^20, 2^20) int literal");
        *type = Type::Intish;
        return f.encoder().writeOp(Op::I32Mul);
    }

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Mul);
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Mul);
    }

    return f.fail(star, "multiply operands must be both int, both double? or both float?");
}

static bool
CheckAddOrSub(FunctionValidator& f, ParseNode* expr, Type* type, unsigned* numAddOrSubOut = nullptr)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    MOZ_ASSERT(expr->isKind(PNK_ADD) || expr->isKind(PNK_SUB));
    ParseNode* lhs = AddSubLeft(expr);
    ParseNode* rhs = AddSubRight(expr);

    Type lhsType, rhsType;
    unsigned lhsNumAddOrSub, rhsNumAddOrSub;

    if (lhs->isKind(PNK_ADD) || lhs->isKind(PNK_SUB)) {
        if (!CheckAddOrSub(f, lhs, &lhsType, &lhsNumAddOrSub))
            return false;
        if (lhsType == Type::Intish)
            lhsType = Type::Int;
    } else {
        if (!CheckExpr(f, lhs, &lhsType))
            return false;
        lhsNumAddOrSub = 0;
    }

    if (rhs->isKind(PNK_ADD) || rhs->isKind(PNK_SUB)) {
        if (!CheckAddOrSub(f, rhs, &rhsType, &rhsNumAddOrSub))
            return false;
        if (rhsType == Type::Intish)
            rhsType = Type::Int;
    } else {
        if (!CheckExpr(f, rhs, &rhsType))
            return false;
        rhsNumAddOrSub = 0;
    }

    unsigned numAddOrSub = lhsNumAddOrSub + rhsNumAddOrSub + 1;
    if (numAddOrSub > (1<<20))
        return f.fail(expr, "too many + or - without intervening coercion");

    if (lhsType.isInt() && rhsType.isInt()) {
        if (!f.encoder().writeOp(expr->isKind(PNK_ADD) ? Op::I32Add : Op::I32Sub))
            return false;
        *type = Type::Intish;
    } else if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        if (!f.encoder().writeOp(expr->isKind(PNK_ADD) ? Op::F64Add : Op::F64Sub))
            return false;
        *type = Type::Double;
    } else if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        if (!f.encoder().writeOp(expr->isKind(PNK_ADD) ? Op::F32Add : Op::F32Sub))
            return false;
        *type = Type::Floatish;
    } else {
        return f.failf(expr, "operands to + or - must both be int, float? or double?, got %s and %s",
                       lhsType.toChars(), rhsType.toChars());
    }

    if (numAddOrSubOut)
        *numAddOrSubOut = numAddOrSub;
    return true;
}

static bool
CheckDivOrMod(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_DIV) || expr->isKind(PNK_MOD));

    ParseNode* lhs = DivOrModLeft(expr);
    ParseNode* rhs = DivOrModRight(expr);

    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(expr->isKind(PNK_DIV) ? Op::F64Div : Op::F64Mod);
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *type = Type::Floatish;
        if (expr->isKind(PNK_DIV))
            return f.encoder().writeOp(Op::F32Div);
        else
            return f.fail(expr, "modulo cannot receive float arguments");
    }

    if (lhsType.isSigned() && rhsType.isSigned()) {
        *type = Type::Intish;
        return f.encoder().writeOp(expr->isKind(PNK_DIV) ? Op::I32DivS : Op::I32RemS);
    }

    if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
        *type = Type::Intish;
        return f.encoder().writeOp(expr->isKind(PNK_DIV) ? Op::I32DivU : Op::I32RemU);
    }

    return f.failf(expr, "arguments to / or %% must both be double?, float?, signed, or unsigned; "
                   "%s and %s are given", lhsType.toChars(), rhsType.toChars());
}

static bool
CheckComparison(FunctionValidator& f, ParseNode* comp, Type* type)
{
    MOZ_ASSERT(comp->isKind(PNK_LT) || comp->isKind(PNK_LE) || comp->isKind(PNK_GT) ||
               comp->isKind(PNK_GE) || comp->isKind(PNK_EQ) || comp->isKind(PNK_NE));

    ParseNode* lhs = ComparisonLeft(comp);
    ParseNode* rhs = ComparisonRight(comp);

    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!(lhsType.isSigned() && rhsType.isSigned()) &&
        !(lhsType.isUnsigned() && rhsType.isUnsigned()) &&
        !(lhsType.isDouble() && rhsType.isDouble()) &&
        !(lhsType.isFloat() && rhsType.isFloat()))
    {
        return f.failf(comp, "arguments to a comparison must both be signed, unsigned, floats or doubles; "
                       "%s and %s are given", lhsType.toChars(), rhsType.toChars());
    }

    Op stmt;
    if (lhsType.isSigned() && rhsType.isSigned()) {
        switch (comp->getOp()) {
          case JSOP_EQ: stmt = Op::I32Eq;  break;
          case JSOP_NE: stmt = Op::I32Ne;  break;
          case JSOP_LT: stmt = Op::I32LtS; break;
          case JSOP_LE: stmt = Op::I32LeS; break;
          case JSOP_GT: stmt = Op::I32GtS; break;
          case JSOP_GE: stmt = Op::I32GeS; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
        switch (comp->getOp()) {
          case JSOP_EQ: stmt = Op::I32Eq;  break;
          case JSOP_NE: stmt = Op::I32Ne;  break;
          case JSOP_LT: stmt = Op::I32LtU; break;
          case JSOP_LE: stmt = Op::I32LeU; break;
          case JSOP_GT: stmt = Op::I32GtU; break;
          case JSOP_GE: stmt = Op::I32GeU; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isDouble()) {
        switch (comp->getOp()) {
          case JSOP_EQ: stmt = Op::F64Eq; break;
          case JSOP_NE: stmt = Op::F64Ne; break;
          case JSOP_LT: stmt = Op::F64Lt; break;
          case JSOP_LE: stmt = Op::F64Le; break;
          case JSOP_GT: stmt = Op::F64Gt; break;
          case JSOP_GE: stmt = Op::F64Ge; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isFloat()) {
        switch (comp->getOp()) {
          case JSOP_EQ: stmt = Op::F32Eq; break;
          case JSOP_NE: stmt = Op::F32Ne; break;
          case JSOP_LT: stmt = Op::F32Lt; break;
          case JSOP_LE: stmt = Op::F32Le; break;
          case JSOP_GT: stmt = Op::F32Gt; break;
          case JSOP_GE: stmt = Op::F32Ge; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else {
        MOZ_CRASH("unexpected type");
    }

    *type = Type::Int;
    return f.encoder().writeOp(stmt);
}

static bool
CheckBitwise(FunctionValidator& f, ParseNode* bitwise, Type* type)
{
    ParseNode* lhs = BitwiseLeft(bitwise);
    ParseNode* rhs = BitwiseRight(bitwise);

    int32_t identityElement;
    bool onlyOnRight;
    switch (bitwise->getKind()) {
      case PNK_BITOR:  identityElement = 0;  onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_BITAND: identityElement = -1; onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_BITXOR: identityElement = 0;  onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_LSH:    identityElement = 0;  onlyOnRight = true;  *type = Type::Signed;   break;
      case PNK_RSH:    identityElement = 0;  onlyOnRight = true;  *type = Type::Signed;   break;
      case PNK_URSH:   identityElement = 0;  onlyOnRight = true;  *type = Type::Unsigned; break;
      default: MOZ_CRASH("not a bitwise op");
    }

    uint32_t i;
    if (!onlyOnRight && IsLiteralInt(f.m(), lhs, &i) && i == uint32_t(identityElement)) {
        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;
        if (!rhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", rhsType.toChars());
        return true;
    }

    if (IsLiteralInt(f.m(), rhs, &i) && i == uint32_t(identityElement)) {
        if (bitwise->isKind(PNK_BITOR) && lhs->isKind(PNK_CALL))
            return CheckCoercedCall(f, lhs, Type::Int, type);

        Type lhsType;
        if (!CheckExpr(f, lhs, &lhsType))
            return false;
        if (!lhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", lhsType.toChars());
        return true;
    }

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    switch (bitwise->getKind()) {
      case PNK_BITOR:  if (!f.encoder().writeOp(Op::I32Or))   return false; break;
      case PNK_BITAND: if (!f.encoder().writeOp(Op::I32And))  return false; break;
      case PNK_BITXOR: if (!f.encoder().writeOp(Op::I32Xor))  return false; break;
      case PNK_LSH:    if (!f.encoder().writeOp(Op::I32Shl))  return false; break;
      case PNK_RSH:    if (!f.encoder().writeOp(Op::I32ShrS)) return false; break;
      case PNK_URSH:   if (!f.encoder().writeOp(Op::I32ShrU)) return false; break;
      default: MOZ_CRASH("not a bitwise op");
    }

    return true;
}

static bool
CheckExpr(FunctionValidator& f, ParseNode* expr, Type* type)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (IsNumericLiteral(f.m(), expr)) {
        return CheckNumericLiteral(f, expr, type);
    }

    switch (expr->getKind()) {
      case PNK_NAME:        return CheckVarRef(f, expr, type);
      case PNK_ELEM:        return CheckLoadArray(f, expr, type);
      case PNK_ASSIGN:      return CheckAssign(f, expr, type);
      case PNK_POS:         return CheckPos(f, expr, type);
      case PNK_NOT:         return CheckNot(f, expr, type);
      case PNK_NEG:         return CheckNeg(f, expr, type);
      case PNK_BITNOT:      return CheckBitNot(f, expr, type);
      case PNK_COMMA:       return CheckComma(f, expr, type);
      case PNK_CONDITIONAL: return CheckConditional(f, expr, type);
      case PNK_STAR:        return CheckMultiply(f, expr, type);
      case PNK_CALL:        return CheckUncoercedCall(f, expr, type);

      case PNK_ADD:
      case PNK_SUB:         return CheckAddOrSub(f, expr, type);

      case PNK_DIV:
      case PNK_MOD:         return CheckDivOrMod(f, expr, type);

      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_EQ:
      case PNK_NE:          return CheckComparison(f, expr, type);

      case PNK_BITOR:
      case PNK_BITAND:
      case PNK_BITXOR:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:        return CheckBitwise(f, expr, type);

      default:;
    }

    return f.fail(expr, "unsupported expression");
}

static bool
CheckStatement(FunctionValidator& f, ParseNode* stmt);

static bool
CheckAsExprStatement(FunctionValidator& f, ParseNode* expr)
{
    if (expr->isKind(PNK_CALL)) {
        Type ignored;
        return CheckCoercedCall(f, expr, Type::Void, &ignored);
    }

    Type resultType;
    if (!CheckExpr(f, expr, &resultType))
        return false;

    if (!resultType.isVoid()) {
        if (!f.encoder().writeOp(Op::Drop))
            return false;
    }

    return true;
}

static bool
CheckExprStatement(FunctionValidator& f, ParseNode* exprStmt)
{
    MOZ_ASSERT(exprStmt->isKind(PNK_SEMI));
    ParseNode* expr = UnaryKid(exprStmt);
    if (!expr)
        return true;
    return CheckAsExprStatement(f, expr);
}

static bool
CheckLoopConditionOnEntry(FunctionValidator& f, ParseNode* cond)
{
    uint32_t maybeLit;
    if (IsLiteralInt(f.m(), cond, &maybeLit) && maybeLit)
        return true;

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    // TODO change this to i32.eqz
    // i32.eq 0 $f
    if (!f.writeInt32Lit(0))
        return false;
    if (!f.encoder().writeOp(Op::I32Eq))
        return false;

    // brIf (i32.eq 0 $f) $out
    if (!f.writeBreakIf())
        return false;

    return true;
}

static bool
CheckWhile(FunctionValidator& f, ParseNode* whileStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(whileStmt->isKind(PNK_WHILE));
    ParseNode* cond = BinaryLeft(whileStmt);
    ParseNode* body = BinaryRight(whileStmt);

    // A while loop `while(#cond) #body` is equivalent to:
    // (block $after_loop
    //    (loop $top
    //       (brIf $after_loop (i32.eq 0 #cond))
    //       #body
    //       (br $top)
    //    )
    // )
    if (labels && !f.addLabels(*labels, 0, 1))
        return false;

    if (!f.pushLoop())
        return false;

    if (!CheckLoopConditionOnEntry(f, cond))
        return false;
    if (!CheckStatement(f, body))
        return false;
    if (!f.writeContinue())
        return false;

    if (!f.popLoop())
        return false;
    if (labels)
        f.removeLabels(*labels);
    return true;
}

static bool
CheckFor(FunctionValidator& f, ParseNode* forStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(forStmt->isKind(PNK_FOR));
    ParseNode* forHead = BinaryLeft(forStmt);
    ParseNode* body = BinaryRight(forStmt);

    if (!forHead->isKind(PNK_FORHEAD))
        return f.fail(forHead, "unsupported for-loop statement");

    ParseNode* maybeInit = TernaryKid1(forHead);
    ParseNode* maybeCond = TernaryKid2(forHead);
    ParseNode* maybeInc = TernaryKid3(forHead);

    // A for-loop `for (#init; #cond; #inc) #body` is equivalent to:
    // (block                                               // depth X
    //   (#init)
    //   (block $after_loop                                 // depth X+1 (block)
    //     (loop $loop_top                                  // depth X+2 (loop)
    //       (brIf $after (eq 0 #cond))
    //       (block $after_body #body)                      // depth X+3
    //       #inc
    //       (br $loop_top)
    //     )
    //   )
    // )
    // A break in the body should break out to $after_loop, i.e. depth + 1.
    // A continue in the body should break out to $after_body, i.e. depth + 3.
    if (labels && !f.addLabels(*labels, 1, 3))
        return false;

    if (!f.pushUnbreakableBlock())
        return false;

    if (maybeInit && !CheckAsExprStatement(f, maybeInit))
        return false;

    {
        if (!f.pushLoop())
            return false;

        if (maybeCond && !CheckLoopConditionOnEntry(f, maybeCond))
            return false;

        {
            // Continuing in the body should just break out to the increment.
            if (!f.pushContinuableBlock())
                return false;
            if (!CheckStatement(f, body))
                return false;
            if (!f.popContinuableBlock())
                return false;
        }

        if (maybeInc && !CheckAsExprStatement(f, maybeInc))
            return false;

        if (!f.writeContinue())
            return false;
        if (!f.popLoop())
            return false;
    }

    if (!f.popUnbreakableBlock())
        return false;

    if (labels)
        f.removeLabels(*labels);

    return true;
}

static bool
CheckDoWhile(FunctionValidator& f, ParseNode* whileStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(whileStmt->isKind(PNK_DOWHILE));
    ParseNode* body = BinaryLeft(whileStmt);
    ParseNode* cond = BinaryRight(whileStmt);

    // A do-while loop `do { #body } while (#cond)` is equivalent to:
    // (block $after_loop           // depth X
    //   (loop $top                 // depth X+1
    //     (block #body)            // depth X+2
    //     (brIf #cond $top)
    //   )
    // )
    // A break should break out of the entire loop, i.e. at depth 0.
    // A continue should break out to the condition, i.e. at depth 2.
    if (labels && !f.addLabels(*labels, 0, 2))
        return false;

    if (!f.pushLoop())
        return false;

    {
        // An unlabeled continue in the body should break out to the condition.
        if (!f.pushContinuableBlock())
            return false;
        if (!CheckStatement(f, body))
            return false;
        if (!f.popContinuableBlock())
            return false;
    }

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    if (!f.writeContinueIf())
        return false;

    if (!f.popLoop())
        return false;
    if (labels)
        f.removeLabels(*labels);
    return true;
}

static bool CheckStatementList(FunctionValidator& f, ParseNode*, const NameVector* = nullptr);

static bool
CheckLabel(FunctionValidator& f, ParseNode* labeledStmt)
{
    MOZ_ASSERT(labeledStmt->isKind(PNK_LABEL));

    NameVector labels;
    ParseNode* innermost = labeledStmt;
    do {
        if (!labels.append(LabeledStatementLabel(innermost)))
            return false;
        innermost = LabeledStatementStatement(innermost);
    } while (innermost->getKind() == PNK_LABEL);

    switch (innermost->getKind()) {
      case PNK_FOR:
        return CheckFor(f, innermost, &labels);
      case PNK_DOWHILE:
        return CheckDoWhile(f, innermost, &labels);
      case PNK_WHILE:
        return CheckWhile(f, innermost, &labels);
      case PNK_STATEMENTLIST:
        return CheckStatementList(f, innermost, &labels);
      default:
        break;
    }

    if (!f.pushUnbreakableBlock(&labels))
        return false;

    if (!CheckStatement(f, innermost))
        return false;

    if (!f.popUnbreakableBlock(&labels))
        return false;
    return true;
}

static bool
CheckIf(FunctionValidator& f, ParseNode* ifStmt)
{
    uint32_t numIfEnd = 1;

  recurse:
    MOZ_ASSERT(ifStmt->isKind(PNK_IF));
    ParseNode* cond = TernaryKid1(ifStmt);
    ParseNode* thenStmt = TernaryKid2(ifStmt);
    ParseNode* elseStmt = TernaryKid3(ifStmt);

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    size_t typeAt;
    if (!f.pushIf(&typeAt))
        return false;

    f.setIfType(typeAt, ExprType::Void);

    if (!CheckStatement(f, thenStmt))
        return false;

    if (elseStmt) {
        if (!f.switchToElse())
            return false;

        if (elseStmt->isKind(PNK_IF)) {
            ifStmt = elseStmt;
            if (numIfEnd++ == UINT32_MAX)
                return false;
            goto recurse;
        }

        if (!CheckStatement(f, elseStmt))
            return false;
    }

    for (uint32_t i = 0; i != numIfEnd; ++i) {
        if (!f.popIf())
            return false;
    }

    return true;
}

static bool
CheckCaseExpr(FunctionValidator& f, ParseNode* caseExpr, int32_t* value)
{
    if (!IsNumericLiteral(f.m(), caseExpr))
        return f.fail(caseExpr, "switch case expression must be an integer literal");

    NumLit lit = ExtractNumericLiteral(f.m(), caseExpr);
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::NegativeInt:
        *value = lit.toInt32();
        break;
      case NumLit::OutOfRangeInt:
      case NumLit::BigUnsigned:
        return f.fail(caseExpr, "switch case expression out of integer range");
      case NumLit::Double:
      case NumLit::Float:
        return f.fail(caseExpr, "switch case expression must be an integer literal");
    }

    return true;
}

static bool
CheckDefaultAtEnd(FunctionValidator& f, ParseNode* stmt)
{
    for (; stmt; stmt = NextNode(stmt)) {
        if (IsDefaultCase(stmt) && NextNode(stmt) != nullptr)
            return f.fail(stmt, "default label must be at the end");
    }

    return true;
}

static bool
CheckSwitchRange(FunctionValidator& f, ParseNode* stmt, int32_t* low, int32_t* high,
                 uint32_t* tableLength)
{
    if (IsDefaultCase(stmt)) {
        *low = 0;
        *high = -1;
        *tableLength = 0;
        return true;
    }

    int32_t i = 0;
    if (!CheckCaseExpr(f, CaseExpr(stmt), &i))
        return false;

    *low = *high = i;

    ParseNode* initialStmt = stmt;
    for (stmt = NextNode(stmt); stmt && !IsDefaultCase(stmt); stmt = NextNode(stmt)) {
        int32_t i = 0;
        if (!CheckCaseExpr(f, CaseExpr(stmt), &i))
            return false;

        *low = Min(*low, i);
        *high = Max(*high, i);
    }

    int64_t i64 = (int64_t(*high) - int64_t(*low)) + 1;
    if (i64 > MaxBrTableElems)
        return f.fail(initialStmt, "all switch statements generate tables; this table would be too big");

    *tableLength = uint32_t(i64);
    return true;
}

static bool
CheckSwitchExpr(FunctionValidator& f, ParseNode* switchExpr)
{
    Type exprType;
    if (!CheckExpr(f, switchExpr, &exprType))
        return false;
    if (!exprType.isSigned())
        return f.failf(switchExpr, "%s is not a subtype of signed", exprType.toChars());
    return true;
}

// A switch will be constructed as:
// - the default block wrapping all the other blocks, to be able to break
// out of the switch with an unlabeled break statement. It has two statements
// (an inner block and the default expr). asm.js rules require default to be at
// the end, so the default block always encloses all the cases blocks.
// - one block per case between low and high; undefined cases just jump to the
// default case. Each of these blocks contain two statements: the next case's
// block and the possibly empty statement list comprising the case body. The
// last block pushed is the first case so the (relative) branch target therefore
// matches the sequential order of cases.
// - one block for the br_table, so that the first break goes to the first
// case's block.
static bool
CheckSwitch(FunctionValidator& f, ParseNode* switchStmt)
{
    MOZ_ASSERT(switchStmt->isKind(PNK_SWITCH));

    ParseNode* switchExpr = BinaryLeft(switchStmt);
    ParseNode* switchBody = BinaryRight(switchStmt);

    if (switchBody->is<LexicalScopeNode>()) {
        LexicalScopeNode* scope = &switchBody->as<LexicalScopeNode>();
        if (!scope->isEmptyScope())
            return f.fail(scope, "switch body may not contain lexical declarations");
        switchBody = scope->scopeBody();
    }

    ParseNode* stmt = ListHead(switchBody);
    if (!stmt) {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
        if (!f.encoder().writeOp(Op::Drop))
            return false;
        return true;
    }

    if (!CheckDefaultAtEnd(f, stmt))
        return false;

    int32_t low = 0, high = 0;
    uint32_t tableLength = 0;
    if (!CheckSwitchRange(f, stmt, &low, &high, &tableLength))
        return false;

    static const uint32_t CASE_NOT_DEFINED = UINT32_MAX;

    Uint32Vector caseDepths;
    if (!caseDepths.appendN(CASE_NOT_DEFINED, tableLength))
        return false;

    uint32_t numCases = 0;
    for (ParseNode* s = stmt; s && !IsDefaultCase(s); s = NextNode(s)) {
        int32_t caseValue = ExtractNumericLiteral(f.m(), CaseExpr(s)).toInt32();

        MOZ_ASSERT(caseValue >= low);
        unsigned i = caseValue - low;
        if (caseDepths[i] != CASE_NOT_DEFINED)
            return f.fail(s, "no duplicate case labels");

        MOZ_ASSERT(numCases != CASE_NOT_DEFINED);
        caseDepths[i] = numCases++;
    }

    // Open the wrapping breakable default block.
    if (!f.pushBreakableBlock())
        return false;

    // Open all the case blocks.
    for (uint32_t i = 0; i < numCases; i++) {
        if (!f.pushUnbreakableBlock())
            return false;
    }

    // Open the br_table block.
    if (!f.pushUnbreakableBlock())
        return false;

    // The default block is the last one.
    uint32_t defaultDepth = numCases;

    // Subtract lowest case value, so that all the cases start from 0.
    if (low) {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
        if (!f.writeInt32Lit(low))
            return false;
        if (!f.encoder().writeOp(Op::I32Sub))
            return false;
    } else {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
    }

    // Start the br_table block.
    if (!f.encoder().writeOp(Op::BrTable))
        return false;

    // Write the number of cases (tableLength - 1 + 1 (default)).
    // Write the number of cases (tableLength - 1 + 1 (default)).
    if (!f.encoder().writeVarU32(tableLength))
        return false;

    // Each case value describes the relative depth to the actual block. When
    // a case is not explicitly defined, it goes to the default.
    for (size_t i = 0; i < tableLength; i++) {
        uint32_t target = caseDepths[i] == CASE_NOT_DEFINED ? defaultDepth : caseDepths[i];
        if (!f.encoder().writeVarU32(target))
            return false;
    }

    // Write the default depth.
    if (!f.encoder().writeVarU32(defaultDepth))
        return false;

    // Our br_table is done. Close its block, write the cases down in order.
    if (!f.popUnbreakableBlock())
        return false;

    for (; stmt && !IsDefaultCase(stmt); stmt = NextNode(stmt)) {
        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
        if (!f.popUnbreakableBlock())
            return false;
    }

    // Write the default block.
    if (stmt && IsDefaultCase(stmt)) {
        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
    }

    // Close the wrapping block.
    if (!f.popBreakableBlock())
        return false;
    return true;
}

static bool
CheckReturnType(FunctionValidator& f, ParseNode* usepn, Type ret)
{
    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(ret.canonicalToExprType());
        return true;
    }

    if (f.returnedType() != ret.canonicalToExprType()) {
        return f.failf(usepn, "%s incompatible with previous return of type %s",
                       Type::ret(ret).toChars(), ToCString(f.returnedType()));
    }

    return true;
}

static bool
CheckReturn(FunctionValidator& f, ParseNode* returnStmt)
{
    ParseNode* expr = ReturnExpr(returnStmt);

    if (!expr) {
        if (!CheckReturnType(f, returnStmt, Type::Void))
            return false;
    } else {
        Type type;
        if (!CheckExpr(f, expr, &type))
            return false;

        if (!type.isReturnType())
            return f.failf(expr, "%s is not a valid return type", type.toChars());

        if (!CheckReturnType(f, expr, Type::canonicalize(type)))
            return false;
    }

    if (!f.encoder().writeOp(Op::Return))
        return false;

    return true;
}

static bool
CheckStatementList(FunctionValidator& f, ParseNode* stmtList, const NameVector* labels /*= nullptr */)
{
    MOZ_ASSERT(stmtList->isKind(PNK_STATEMENTLIST));

    if (!f.pushUnbreakableBlock(labels))
        return false;

    for (ParseNode* stmt = ListHead(stmtList); stmt; stmt = NextNode(stmt)) {
        if (!CheckStatement(f, stmt))
            return false;
    }

    if (!f.popUnbreakableBlock(labels))
        return false;
    return true;
}

static bool
CheckLexicalScope(FunctionValidator& f, ParseNode* node)
{
    LexicalScopeNode* lexicalScope = &node->as<LexicalScopeNode>();
    if (!lexicalScope->isEmptyScope())
        return f.fail(lexicalScope, "cannot have 'let' or 'const' declarations");

    return CheckStatement(f, lexicalScope->scopeBody());
}

static bool
CheckBreakOrContinue(FunctionValidator& f, bool isBreak, ParseNode* stmt)
{
    if (PropertyName* maybeLabel = LoopControlMaybeLabel(stmt))
        return f.writeLabeledBreakOrContinue(maybeLabel, isBreak);
    return f.writeUnlabeledBreakOrContinue(isBreak);
}

static bool
CheckStatement(FunctionValidator& f, ParseNode* stmt)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    switch (stmt->getKind()) {
      case PNK_SEMI:          return CheckExprStatement(f, stmt);
      case PNK_WHILE:         return CheckWhile(f, stmt);
      case PNK_FOR:           return CheckFor(f, stmt);
      case PNK_DOWHILE:       return CheckDoWhile(f, stmt);
      case PNK_LABEL:         return CheckLabel(f, stmt);
      case PNK_IF:            return CheckIf(f, stmt);
      case PNK_SWITCH:        return CheckSwitch(f, stmt);
      case PNK_RETURN:        return CheckReturn(f, stmt);
      case PNK_STATEMENTLIST: return CheckStatementList(f, stmt);
      case PNK_BREAK:         return CheckBreakOrContinue(f, true, stmt);
      case PNK_CONTINUE:      return CheckBreakOrContinue(f, false, stmt);
      case PNK_LEXICALSCOPE:  return CheckLexicalScope(f, stmt);
      default:;
    }

    return f.fail(stmt, "unexpected statement kind");
}

static bool
ParseFunction(ModuleValidator& m, FunctionNode** funNodeOut, unsigned* line)
{
    TokenStream& tokenStream = m.tokenStream();

    tokenStream.consumeKnownToken(TOK_FUNCTION, TokenStream::Operand);
    uint32_t toStringStart = tokenStream.currentToken().pos.begin;
    *line = tokenStream.srcCoords.lineNum(tokenStream.currentToken().pos.end);

    TokenKind tk;
    if (!tokenStream.getToken(&tk, TokenStream::Operand))
        return false;
    if (!TokenKindIsPossibleIdentifier(tk))
        return false;  // The regular parser will throw a SyntaxError, no need to m.fail.

    RootedPropertyName name(m.cx(), m.parser().bindingIdentifier(YieldIsName));
    if (!name)
        return false;

    FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;
    FunctionNode* funNode = m.parser().handler.newFunction(syntaxKind, m.parser().pos());
    if (!funNode)
        return false;

    RootedFunction& fun = m.dummyFunction();
    fun->setAtom(name);
    fun->setArgCount(0);

    ParseContext* outerpc = m.parser().pc;
    Directives directives(outerpc);
    FunctionBox* funbox = m.parser().newFunctionBox(funNode, fun, toStringStart, directives, NotGenerator,
                                                    SyncFunction, /* tryAnnexB = */ false);
    if (!funbox)
        return false;
    funbox->initWithEnclosingParseContext(outerpc, syntaxKind);

    Directives newDirectives = directives;
    ParseContext funpc(&m.parser(), funbox, &newDirectives);
    if (!funpc.init())
        return false;

    if (!m.parser().functionFormalParametersAndBody(InAllowed, YieldIsName, funNode, syntaxKind)) {
        if (tokenStream.hadError() || directives == newDirectives)
            return false;

        return m.fail(funNode, "encountered new directive in function");
    }

    MOZ_ASSERT(!tokenStream.hadError());
    MOZ_ASSERT(directives == newDirectives);

    *funNodeOut = funNode;
    return true;
}

static bool
CheckFunction(ModuleValidator& m)
{
    // asm.js modules can be quite large when represented as parse trees so pop
    // the backing LifoAlloc after parsing/compiling each function.
    AsmJSParser::Mark mark = m.parser().mark();

    FunctionNode* funNode = nullptr;
    unsigned line = 0;
    if (!ParseFunction(m, &funNode, &line))
        return false;

    if (!CheckFunctionHead(m, funNode))
        return false;

    FunctionValidator f(m, funNode);
    if (!f.init(FunctionName(funNode), line))
        return m.fail(funNode, "internal compiler failure (probably out of memory)");

    ParseNode* stmtIter = ListHead(FunctionStatementList(funNode));

    if (!CheckProcessingDirectives(m, &stmtIter))
        return false;

    ValTypeVector args;
    if (!CheckArguments(f, &stmtIter, &args))
        return false;

    if (!CheckVariables(f, &stmtIter))
        return false;

    ParseNode* lastNonEmptyStmt = nullptr;
    for (; stmtIter; stmtIter = NextNonEmptyStatement(stmtIter)) {
        lastNonEmptyStmt = stmtIter;
        if (!CheckStatement(f, stmtIter))
            return false;
    }

    if (!CheckFinalReturn(f, lastNonEmptyStmt))
        return false;

    ModuleValidator::Func* func = nullptr;
    if (!CheckFunctionSignature(m, funNode, Sig(Move(args), f.returnedType()), FunctionName(funNode), &func))
        return false;

    if (func->defined())
        return m.failName(funNode, "function '%s' already defined", FunctionName(funNode));

    func->define(funNode);

    if (!f.finish(func->index()))
        return m.fail(funNode, "internal compiler failure (probably out of memory)");

    // Release the parser's lifo memory only after the last use of a parse node.
    m.parser().release(mark);
    return true;
}

static bool
CheckAllFunctionsDefined(ModuleValidator& m)
{
    for (unsigned i = 0; i < m.numFunctions(); i++) {
        ModuleValidator::Func& f = m.function(i);
        if (!f.defined())
            return m.failNameOffset(f.firstUse(), "missing definition of function %s", f.name());
    }

    return true;
}

static bool
CheckFunctions(ModuleValidator& m)
{
    while (true) {
        TokenKind tk;
        if (!PeekToken(m.parser(), &tk))
            return false;

        if (tk != TOK_FUNCTION)
            break;

        if (!CheckFunction(m))
            return false;
    }

    return CheckAllFunctionsDefined(m);
}

static bool
CheckFuncPtrTable(ModuleValidator& m, ParseNode* var)
{
    if (!var->isKind(PNK_NAME))
        return m.fail(var, "function-pointer table name is not a plain name");

    ParseNode* arrayLiteral = MaybeInitializer(var);
    if (!arrayLiteral || !arrayLiteral->isKind(PNK_ARRAY))
        return m.fail(var, "function-pointer table's initializer must be an array literal");

    unsigned length = ListLength(arrayLiteral);

    if (!IsPowerOfTwo(length))
        return m.failf(arrayLiteral, "function-pointer table length must be a power of 2 (is %u)", length);

    unsigned mask = length - 1;

    Uint32Vector elemFuncIndices;
    const Sig* sig = nullptr;
    for (ParseNode* elem = ListHead(arrayLiteral); elem; elem = NextNode(elem)) {
        if (!elem->isKind(PNK_NAME))
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        PropertyName* funcName = elem->name();
        const ModuleValidator::Func* func = m.lookupFunction(funcName);
        if (!func)
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        const Sig& funcSig = m.mg().funcSig(func->index());
        if (sig) {
            if (*sig != funcSig)
                return m.fail(elem, "all functions in table must have same signature");
        } else {
            sig = &funcSig;
        }

        if (!elemFuncIndices.append(func->index()))
            return false;
    }

    Sig copy;
    if (!copy.clone(*sig))
        return false;

    uint32_t tableIndex;
    if (!CheckFuncPtrTableAgainstExisting(m, var, var->name(), Move(copy), mask, &tableIndex))
        return false;

    if (!m.defineFuncPtrTable(tableIndex, Move(elemFuncIndices)))
        return m.fail(var, "duplicate function-pointer definition");

    return true;
}

static bool
CheckFuncPtrTables(ModuleValidator& m)
{
    while (true) {
        ParseNode* varStmt;
        if (!ParseVarOrConstStatement(m.parser(), &varStmt))
            return false;
        if (!varStmt)
            break;
        for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
            if (!CheckFuncPtrTable(m, var))
                return false;
        }
    }

    for (unsigned i = 0; i < m.numFuncPtrTables(); i++) {
        ModuleValidator::FuncPtrTable& funcPtrTable = m.funcPtrTable(i);
        if (!funcPtrTable.defined()) {
            return m.failNameOffset(funcPtrTable.firstUse(),
                                    "function-pointer table %s wasn't defined",
                                    funcPtrTable.name());
        }
    }

    return true;
}

static bool
CheckModuleExportFunction(ModuleValidator& m, ParseNode* pn, PropertyName* maybeFieldName = nullptr)
{
    if (!pn->isKind(PNK_NAME))
        return m.fail(pn, "expected name of exported function");

    PropertyName* funcName = pn->name();
    const ModuleValidator::Func* func = m.lookupFunction(funcName);
    if (!func)
        return m.failName(pn, "function '%s' not found", funcName);

    return m.addExportField(pn, *func, maybeFieldName);
}

static bool
CheckModuleExportObject(ModuleValidator& m, ParseNode* object)
{
    MOZ_ASSERT(object->isKind(PNK_OBJECT));

    for (ParseNode* pn = ListHead(object); pn; pn = NextNode(pn)) {
        if (!IsNormalObjectField(m.cx(), pn))
            return m.fail(pn, "only normal object properties may be used in the export object literal");

        PropertyName* fieldName = ObjectNormalFieldName(m.cx(), pn);

        ParseNode* initNode = ObjectNormalFieldInitializer(m.cx(), pn);
        if (!initNode->isKind(PNK_NAME))
            return m.fail(initNode, "initializer of exported object literal must be name of function");

        if (!CheckModuleExportFunction(m, initNode, fieldName))
            return false;
    }

    return true;
}

static bool
CheckModuleReturn(ModuleValidator& m)
{
    TokenKind tk;
    if (!GetToken(m.parser(), &tk))
        return false;
    TokenStream& ts = m.parser().tokenStream;
    if (tk != TOK_RETURN) {
        return m.failCurrentOffset((tk == TOK_RC || tk == TOK_EOF)
                                   ? "expecting return statement"
                                   : "invalid asm.js. statement");
    }
    ts.ungetToken();

    ParseNode* returnStmt = m.parser().statementListItem(YieldIsName);
    if (!returnStmt)
        return false;

    ParseNode* returnExpr = ReturnExpr(returnStmt);
    if (!returnExpr)
        return m.fail(returnStmt, "export statement must return something");

    if (returnExpr->isKind(PNK_OBJECT)) {
        if (!CheckModuleExportObject(m, returnExpr))
            return false;
    } else {
        if (!CheckModuleExportFunction(m, returnExpr))
            return false;
    }

    return true;
}

static bool
CheckModuleEnd(ModuleValidator &m)
{
    TokenKind tk;
    if (!GetToken(m.parser(), &tk))
        return false;

    if (tk != TOK_EOF && tk != TOK_RC)
        return m.failCurrentOffset("top-level export (return) must be the last statement");

    m.parser().tokenStream.ungetToken();
    return true;
}

static SharedModule
CheckModule(ExclusiveContext* cx, AsmJSParser& parser, ParseNode* stmtList, unsigned* time)
{
    int64_t before = PRMJ_Now();

    FunctionNode* moduleFunctionNode = parser.pc->functionBox()->functionNode;

    ModuleValidator m(cx, parser, moduleFunctionNode);
    if (!m.init())
        return nullptr;

    if (!CheckFunctionHead(m, moduleFunctionNode))
        return nullptr;

    if (!CheckModuleArguments(m, moduleFunctionNode))
        return nullptr;

    if (!CheckPrecedingStatements(m, stmtList))
        return nullptr;

    if (!CheckModuleProcessingDirectives(m))
        return nullptr;

    if (!CheckModuleGlobals(m))
        return nullptr;

    if (!m.startFunctionBodies())
        return nullptr;

    if (!CheckFunctions(m))
        return nullptr;

    if (!m.finishFunctionBodies())
        return nullptr;

    if (!CheckFuncPtrTables(m))
        return nullptr;

    if (!CheckModuleReturn(m))
        return nullptr;

    if (!CheckModuleEnd(m))
        return nullptr;

    SharedModule module = m.finish();
    if (!module)
        return nullptr;

    *time = (PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC;
    return module;
}

/*****************************************************************************/
// Link-time validation

static bool
LinkFail(JSContext* cx, const char* str)
{
    JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                      JSMSG_USE_ASM_LINK_FAIL, str);
    return false;
}

static bool
IsMaybeWrappedScriptedProxy(JSObject* obj)
{
    JSObject* unwrapped = UncheckedUnwrap(obj);
    return unwrapped && IsScriptedProxy(unwrapped);
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, HandleAtom field, MutableHandleValue v)
{
    if (!objVal.isObject())
        return LinkFail(cx, "accessing property of non-object");

    RootedObject obj(cx, &objVal.toObject());
    if (IsMaybeWrappedScriptedProxy(obj))
        return LinkFail(cx, "accessing property of a Proxy");

    Rooted<PropertyDescriptor> desc(cx);
    RootedId id(cx, AtomToId(field));
    if (!GetPropertyDescriptor(cx, obj, id, &desc))
        return false;

    if (!desc.object())
        return LinkFail(cx, "property not present on object");

    if (!desc.isDataDescriptor())
        return LinkFail(cx, "property is not a data property");

    v.set(desc.value());
    return true;
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, const char* fieldChars, MutableHandleValue v)
{
    RootedAtom field(cx, AtomizeUTF8Chars(cx, fieldChars, strlen(fieldChars)));
    if (!field)
        return false;

    return GetDataProperty(cx, objVal, field, v);
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, ImmutablePropertyNamePtr field, MutableHandleValue v)
{
    // Help the conversion along for all the cx->names().* users.
    HandlePropertyName fieldHandle = field;
    return GetDataProperty(cx, objVal, fieldHandle, v);
}

static bool
HasObjectValueOfMethodPure(JSObject* obj, JSContext* cx)
{
    Value v;
    if (!GetPropertyPure(cx, obj, NameToId(cx->names().valueOf), &v))
        return false;

    JSFunction* fun;
    if (!IsFunctionObject(v, &fun))
        return false;

    return IsSelfHostedFunctionWithName(fun, cx->names().Object_valueOf);
}

static bool
HasPureCoercion(JSContext* cx, HandleValue v)
{
    // Ideally, we'd reject all non-primitives, but Emscripten has a bug that
    // generates code that passes functions for some imports. To avoid breaking
    // all the code that contains this bug, we make an exception for functions
    // that don't have user-defined valueOf or toString, for their coercions
    // are not observable and coercion via ToNumber/ToInt32 definitely produces
    // NaN/0. We should remove this special case later once most apps have been
    // built with newer Emscripten.
    if (v.toObject().is<JSFunction>() &&
        HasNoToPrimitiveMethodPure(&v.toObject(), cx) &&
        HasObjectValueOfMethodPure(&v.toObject(), cx) &&
        HasNativeMethodPure(&v.toObject(), cx->names().toString, fun_toString, cx))
    {
        return true;
    }

    return false;
}

static bool
ValidateGlobalVariable(JSContext* cx, const AsmJSGlobal& global, HandleValue importVal, Val* val)
{
    switch (global.varInitKind()) {
      case AsmJSGlobal::InitConstant:
        *val = global.varInitVal();
        return true;

      case AsmJSGlobal::InitImport: {
        RootedValue v(cx);
        if (!GetDataProperty(cx, importVal, global.field(), &v))
            return false;

        if (!v.isPrimitive() && !HasPureCoercion(cx, v))
            return LinkFail(cx, "Imported values must be primitives");

        switch (global.varInitImportType()) {
          case ValType::I32: {
            int32_t i32;
            if (!ToInt32(cx, v, &i32))
                return false;
            *val = Val(uint32_t(i32));
            return true;
          }
          case ValType::I64:
            MOZ_CRASH("int64");
          case ValType::F32: {
            float f;
            if (!RoundFloat32(cx, v, &f))
                return false;
            *val = Val(f);
            return true;
          }
          case ValType::F64: {
            double d;
            if (!ToNumber(cx, v, &d))
                return false;
            *val = Val(d);
            return true;
          }
        }
      }
    }

    MOZ_CRASH("unreachable");
}

static bool
ValidateFFI(JSContext* cx, const AsmJSGlobal& global, HandleValue importVal,
            MutableHandle<FunctionVector> ffis)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, importVal, global.field(), &v))
        return false;

    if (!IsFunctionObject(v))
        return LinkFail(cx, "FFI imports must be functions");

    ffis[global.ffiIndex()].set(&v.toObject().as<JSFunction>());
    return true;
}

static bool
ValidateArrayView(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    if (!global.field())
        return true;

    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, global.field(), &v))
        return false;

    bool tac = IsTypedArrayConstructor(v, global.viewType());
    if (!tac)
        return LinkFail(cx, "bad typed array constructor");

    return true;
}

static bool
ValidateMathBuiltinFunction(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Math, &v))
        return false;

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    Native native = nullptr;
    switch (global.mathBuiltinFunction()) {
      case AsmJSMathBuiltin_sin: native = math_sin; break;
      case AsmJSMathBuiltin_cos: native = math_cos; break;
      case AsmJSMathBuiltin_tan: native = math_tan; break;
      case AsmJSMathBuiltin_asin: native = math_asin; break;
      case AsmJSMathBuiltin_acos: native = math_acos; break;
      case AsmJSMathBuiltin_atan: native = math_atan; break;
      case AsmJSMathBuiltin_ceil: native = math_ceil; break;
      case AsmJSMathBuiltin_floor: native = math_floor; break;
      case AsmJSMathBuiltin_exp: native = math_exp; break;
      case AsmJSMathBuiltin_log: native = math_log; break;
      case AsmJSMathBuiltin_pow: native = math_pow; break;
      case AsmJSMathBuiltin_sqrt: native = math_sqrt; break;
      case AsmJSMathBuiltin_min: native = math_min; break;
      case AsmJSMathBuiltin_max: native = math_max; break;
      case AsmJSMathBuiltin_abs: native = math_abs; break;
      case AsmJSMathBuiltin_atan2: native = math_atan2; break;
      case AsmJSMathBuiltin_imul: native = math_imul; break;
      case AsmJSMathBuiltin_clz32: native = math_clz32; break;
      case AsmJSMathBuiltin_fround: native = math_fround; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Math.* builtin function");

    return true;
}

static bool
ValidateAtomicsBuiltinFunction(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Atomics, &v))
        return false;

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    Native native = nullptr;
    switch (global.atomicsBuiltinFunction()) {
      case AsmJSAtomicsBuiltin_compareExchange: native = atomics_compareExchange; break;
      case AsmJSAtomicsBuiltin_exchange: native = atomics_exchange; break;
      case AsmJSAtomicsBuiltin_load: native = atomics_load; break;
      case AsmJSAtomicsBuiltin_store: native = atomics_store; break;
      case AsmJSAtomicsBuiltin_add: native = atomics_add; break;
      case AsmJSAtomicsBuiltin_sub: native = atomics_sub; break;
      case AsmJSAtomicsBuiltin_and: native = atomics_and; break;
      case AsmJSAtomicsBuiltin_or: native = atomics_or; break;
      case AsmJSAtomicsBuiltin_xor: native = atomics_xor; break;
      case AsmJSAtomicsBuiltin_isLockFree: native = atomics_isLockFree; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Atomics.* builtin function");

    return true;
}

static bool
ValidateConstant(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx, globalVal);

    if (global.constantKind() == AsmJSGlobal::MathConstant) {
        if (!GetDataProperty(cx, v, cx->names().Math, &v))
            return false;
    }

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    if (!v.isNumber())
        return LinkFail(cx, "math / global constant value needs to be a number");

    // NaN != NaN
    if (IsNaN(global.constantValue())) {
        if (!IsNaN(v.toNumber()))
            return LinkFail(cx, "global constant value needs to be NaN");
    } else {
        if (v.toNumber() != global.constantValue())
            return LinkFail(cx, "global constant value mismatch");
    }

    return true;
}

static bool
CheckBuffer(JSContext* cx, const AsmJSMetadata& metadata, HandleValue bufferVal,
            MutableHandle<ArrayBufferObjectMaybeShared*> buffer)
{
    if (metadata.memoryUsage == MemoryUsage::Shared) {
        if (!IsSharedArrayBuffer(bufferVal))
            return LinkFail(cx, "shared views can only be constructed onto SharedArrayBuffer");
    } else {
        if (!IsArrayBuffer(bufferVal))
            return LinkFail(cx, "unshared views can only be constructed onto ArrayBuffer");
    }

    buffer.set(&AsAnyArrayBuffer(bufferVal));
    uint32_t memoryLength = buffer->byteLength();

    if (!IsValidAsmJSHeapLength(memoryLength)) {
        UniqueChars msg(
            JS_smprintf("ArrayBuffer byteLength 0x%x is not a valid heap length. The next "
                        "valid length is 0x%x",
                        memoryLength,
                        RoundUpToNextValidAsmJSHeapLength(memoryLength)));
        if (!msg)
            return false;
        return LinkFail(cx, msg.get());
    }

    // This check is sufficient without considering the size of the loaded datum because heap
    // loads and stores start on an aligned boundary and the heap byteLength has larger alignment.
    MOZ_ASSERT((metadata.minMemoryLength - 1) <= INT32_MAX);
    if (memoryLength < metadata.minMemoryLength) {
        UniqueChars msg(
            JS_smprintf("ArrayBuffer byteLength of 0x%x is less than 0x%x (the size implied "
                        "by const heap accesses).",
                        memoryLength,
                        metadata.minMemoryLength));
        if (!msg)
            return false;
        return LinkFail(cx, msg.get());
    }

    if (buffer->is<ArrayBufferObject>()) {
        // On 64-bit, bounds checks are statically removed so the huge guard
        // region is always necessary. On 32-bit, allocating a guard page
        // requires reallocating the incoming ArrayBuffer which could trigger
        // OOM. Thus, don't ask for a guard page in this case.
#ifdef WASM_HUGE_MEMORY
        bool needGuard = true;
#else
        bool needGuard = false;
#endif
        Rooted<ArrayBufferObject*> arrayBuffer(cx, &buffer->as<ArrayBufferObject>());
        if (!ArrayBufferObject::prepareForAsmJS(cx, arrayBuffer, needGuard))
            return LinkFail(cx, "Unable to prepare ArrayBuffer for asm.js use");
    } else {
        if (!buffer->as<SharedArrayBufferObject>().isPreparedForAsmJS())
            return LinkFail(cx, "SharedArrayBuffer must be created with wasm test mode enabled");
    }

    MOZ_ASSERT(buffer->isPreparedForAsmJS());
    return true;
}

static bool
GetImports(JSContext* cx, const AsmJSMetadata& metadata, HandleValue globalVal,
           HandleValue importVal, MutableHandle<FunctionVector> funcImports, ValVector* valImports)
{
    Rooted<FunctionVector> ffis(cx, FunctionVector(cx));
    if (!ffis.resize(metadata.numFFIs))
        return false;

    for (const AsmJSGlobal& global : metadata.asmJSGlobals) {
        switch (global.which()) {
          case AsmJSGlobal::Variable: {
            Val val;
            if (!ValidateGlobalVariable(cx, global, importVal, &val))
                return false;
            if (!valImports->append(val))
                return false;
            break;
          }
          case AsmJSGlobal::FFI:
            if (!ValidateFFI(cx, global, importVal, &ffis))
                return false;
            break;
          case AsmJSGlobal::ArrayView:
          case AsmJSGlobal::ArrayViewCtor:
            if (!ValidateArrayView(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::MathBuiltinFunction:
            if (!ValidateMathBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::AtomicsBuiltinFunction:
            if (!ValidateAtomicsBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::Constant:
            if (!ValidateConstant(cx, global, globalVal))
                return false;
            break;
        }
    }

    for (const AsmJSImport& import : metadata.asmJSImports) {
        if (!funcImports.append(ffis[import.ffiIndex()]))
            return false;
    }

    return true;
}

static bool
TryInstantiate(JSContext* cx, CallArgs args, Module& module, const AsmJSMetadata& metadata,
               MutableHandleWasmInstanceObject instanceObj, MutableHandleObject exportObj)
{
    HandleValue globalVal = args.get(0);
    HandleValue importVal = args.get(1);
    HandleValue bufferVal = args.get(2);

    RootedArrayBufferObjectMaybeShared buffer(cx);
    RootedWasmMemoryObject memory(cx);
    if (module.metadata().usesMemory()) {
        if (!CheckBuffer(cx, metadata, bufferVal, &buffer))
            return false;

        memory = WasmMemoryObject::create(cx, buffer, nullptr);
        if (!memory)
            return false;
    }

    ValVector valImports;
    Rooted<FunctionVector> funcs(cx, FunctionVector(cx));
    if (!GetImports(cx, metadata, globalVal, importVal, &funcs, &valImports))
        return false;

    RootedWasmTableObject table(cx);
    if (!module.instantiate(cx, funcs, table, memory, valImports, nullptr, instanceObj))
        return false;

    RootedValue exportObjVal(cx);
    if (!JS_GetProperty(cx, instanceObj, InstanceExportField, &exportObjVal))
        return false;

    MOZ_RELEASE_ASSERT(exportObjVal.isObject());
    exportObj.set(&exportObjVal.toObject());
    return true;
}

static bool
HandleInstantiationFailure(JSContext* cx, CallArgs args, const AsmJSMetadata& metadata)
{
    RootedAtom name(cx, args.callee().as<JSFunction>().explicitName());

    if (cx->isExceptionPending())
        return false;

    ScriptSource* source = metadata.scriptSource.get();

    // Source discarding is allowed to affect JS semantics because it is never
    // enabled for normal JS content.
    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return false;
    if (!haveSource) {
        JS_ReportErrorASCII(cx, "asm.js link failure with source discarding enabled");
        return false;
    }

    uint32_t begin = metadata.toStringStart;
    uint32_t end = metadata.srcEndAfterCurly();
    Rooted<JSFlatString*> src(cx, source->substringDontDeflate(cx, begin, end));
    if (!src)
        return false;

    RootedFunction fun(cx, NewScriptedFunction(cx, 0, JSFunction::INTERPRETED_NORMAL,
                                               name, /* proto = */ nullptr, gc::AllocKind::FUNCTION,
                                               TenuredObject));
    if (!fun)
        return false;

    CompileOptions options(cx);
    options.setMutedErrors(source->mutedErrors())
           .setFile(source->filename())
           .setNoScriptRval(false);
    options.asmJSOption = AsmJSOption::Disabled;

    // The exported function inherits an implicit strict context if the module
    // also inherited it somehow.
    if (metadata.strict)
        options.strictOption = true;

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, src))
        return false;

    const char16_t* chars = stableChars.twoByteRange().begin().get();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    SourceBufferHolder srcBuf(chars, end - begin, ownership);
    if (!frontend::CompileStandaloneFunction(cx, &fun, options, srcBuf, Nothing()))
        return false;

    // Call the function we just recompiled.
    args.setCallee(ObjectValue(*fun));
    return InternalCallOrConstruct(cx, args, args.isConstructing() ? CONSTRUCT : NO_CONSTRUCT);
}

static Module&
AsmJSModuleFunctionToModule(JSFunction* fun)
{
    MOZ_ASSERT(IsAsmJSModule(fun));
    const Value& v = fun->getExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT);
    return v.toObject().as<WasmModuleObject>().module();
}

// Implements the semantics of an asm.js module function that has been successfully validated.
bool
js::InstantiateAsmJS(JSContext* cx, unsigned argc, JS::Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunction* callee = &args.callee().as<JSFunction>();
    Module& module = AsmJSModuleFunctionToModule(callee);
    const AsmJSMetadata& metadata = module.metadata().asAsmJS();

    RootedWasmInstanceObject instanceObj(cx);
    RootedObject exportObj(cx);
    if (!TryInstantiate(cx, args, module, metadata, &instanceObj, &exportObj)) {
        // Link-time validation checks failed, so reparse the entire asm.js
        // module from scratch to get normal interpreted bytecode which we can
        // simply Invoke. Very slow.
        return HandleInstantiationFailure(cx, args, metadata);
    }

    args.rval().set(ObjectValue(*exportObj));
    return true;
}

static JSFunction*
NewAsmJSModuleFunction(ExclusiveContext* cx, JSFunction* origFun, HandleObject moduleObj)
{
    RootedAtom name(cx, origFun->explicitName());

    JSFunction::Flags flags = origFun->isLambda() ? JSFunction::ASMJS_LAMBDA_CTOR
                                                  : JSFunction::ASMJS_CTOR;
    JSFunction* moduleFun =
        NewNativeConstructor(cx, InstantiateAsmJS, origFun->nargs(), name,
                             gc::AllocKind::FUNCTION_EXTENDED, TenuredObject,
                             flags);
    if (!moduleFun)
        return nullptr;

    moduleFun->setExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT, ObjectValue(*moduleObj));

    MOZ_ASSERT(IsAsmJSModule(moduleFun));
    return moduleFun;
}

/*****************************************************************************/
// Caching and cloning

size_t
AsmJSGlobal::serializedSize() const
{
    return sizeof(pod) +
           field_.serializedSize();
}

uint8_t*
AsmJSGlobal::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = field_.serialize(cursor);
    return cursor;
}

const uint8_t*
AsmJSGlobal::deserialize(const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (cursor = field_.deserialize(cursor));
    return cursor;
}

size_t
AsmJSGlobal::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return field_.sizeOfExcludingThis(mallocSizeOf);
}

size_t
AsmJSMetadata::serializedSize() const
{
    return Metadata::serializedSize() +
           sizeof(pod()) +
           SerializedVectorSize(asmJSGlobals) +
           SerializedPodVectorSize(asmJSImports) +
           SerializedPodVectorSize(asmJSExports) +
           SerializedVectorSize(asmJSFuncNames) +
           globalArgumentName.serializedSize() +
           importArgumentName.serializedSize() +
           bufferArgumentName.serializedSize();
}

uint8_t*
AsmJSMetadata::serialize(uint8_t* cursor) const
{
    cursor = Metadata::serialize(cursor);
    cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
    cursor = SerializeVector(cursor, asmJSGlobals);
    cursor = SerializePodVector(cursor, asmJSImports);
    cursor = SerializePodVector(cursor, asmJSExports);
    cursor = SerializeVector(cursor, asmJSFuncNames);
    cursor = globalArgumentName.serialize(cursor);
    cursor = importArgumentName.serialize(cursor);
    cursor = bufferArgumentName.serialize(cursor);
    return cursor;
}

const uint8_t*
AsmJSMetadata::deserialize(const uint8_t* cursor)
{
    (cursor = Metadata::deserialize(cursor)) &&
    (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
    (cursor = DeserializeVector(cursor, &asmJSGlobals)) &&
    (cursor = DeserializePodVector(cursor, &asmJSImports)) &&
    (cursor = DeserializePodVector(cursor, &asmJSExports)) &&
    (cursor = DeserializeVector(cursor, &asmJSFuncNames)) &&
    (cursor = globalArgumentName.deserialize(cursor)) &&
    (cursor = importArgumentName.deserialize(cursor)) &&
    (cursor = bufferArgumentName.deserialize(cursor));
    cacheResult = CacheResult::Hit;
    return cursor;
}

size_t
AsmJSMetadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return Metadata::sizeOfExcludingThis(mallocSizeOf) +
           SizeOfVectorExcludingThis(asmJSGlobals, mallocSizeOf) +
           asmJSImports.sizeOfExcludingThis(mallocSizeOf) +
           asmJSExports.sizeOfExcludingThis(mallocSizeOf) +
           SizeOfVectorExcludingThis(asmJSFuncNames, mallocSizeOf) +
           globalArgumentName.sizeOfExcludingThis(mallocSizeOf) +
           importArgumentName.sizeOfExcludingThis(mallocSizeOf) +
           bufferArgumentName.sizeOfExcludingThis(mallocSizeOf);
}

namespace {

class ModuleChars
{
  protected:
    uint32_t isFunCtor_;
    Vector<CacheableChars, 0, SystemAllocPolicy> funCtorArgs_;

  public:
    static uint32_t beginOffset(AsmJSParser& parser) {
        return parser.pc->functionBox()->functionNode->pn_pos.begin;
    }

    static uint32_t endOffset(AsmJSParser& parser) {
        TokenPos pos(0, 0);  // initialize to silence GCC warning
        MOZ_ALWAYS_TRUE(parser.tokenStream.peekTokenPos(&pos, TokenStream::Operand));
        return pos.end;
    }
};

class ModuleCharsForStore : ModuleChars
{
    uint32_t uncompressedSize_;
    uint32_t compressedSize_;
    Vector<char, 0, SystemAllocPolicy> compressedBuffer_;

  public:
    bool init(AsmJSParser& parser) {
        MOZ_ASSERT(beginOffset(parser) < endOffset(parser));

        uncompressedSize_ = (endOffset(parser) - beginOffset(parser)) * sizeof(char16_t);
        size_t maxCompressedSize = LZ4::maxCompressedSize(uncompressedSize_);
        if (maxCompressedSize < uncompressedSize_)
            return false;

        if (!compressedBuffer_.resize(maxCompressedSize))
            return false;

        const char16_t* chars = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char* source = reinterpret_cast<const char*>(chars);
        size_t compressedSize = LZ4::compress(source, uncompressedSize_, compressedBuffer_.begin());
        if (!compressedSize || compressedSize > UINT32_MAX)
            return false;

        compressedSize_ = compressedSize;

        // For a function statement or named function expression:
        //   function f(x,y,z) { abc }
        // the range [beginOffset, endOffset) captures the source:
        //   f(x,y,z) { abc }
        // An unnamed function expression captures the same thing, sans 'f'.
        // Since asm.js modules do not contain any free variables, equality of
        // [beginOffset, endOffset) is sufficient to guarantee identical code
        // generation, modulo Assumptions.
        //
        // For functions created with 'new Function', function arguments are
        // not present in the source so we must manually explicitly serialize
        // and match the formals as a Vector of PropertyName.
        isFunCtor_ = parser.pc->isStandaloneFunctionBody();
        if (isFunCtor_) {
            unsigned numArgs;
            FunctionNode* functionNode = parser.pc->functionBox()->functionNode;
            ParseNode* arg = FunctionFormalParametersList(functionNode, &numArgs);
            for (unsigned i = 0; i < numArgs; i++, arg = arg->pn_next) {
                UniqueChars name = StringToNewUTF8CharsZ(nullptr, *arg->name());
                if (!name || !funCtorArgs_.append(Move(name)))
                    return false;
            }
        }

        return true;
    }

    size_t serializedSize() const {
        return sizeof(uint32_t) +
               sizeof(uint32_t) +
               compressedSize_ +
               sizeof(uint32_t) +
               (isFunCtor_ ? SerializedVectorSize(funCtorArgs_) : 0);
    }

    uint8_t* serialize(uint8_t* cursor) const {
        cursor = WriteScalar<uint32_t>(cursor, uncompressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, compressedSize_);
        cursor = WriteBytes(cursor, compressedBuffer_.begin(), compressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, isFunCtor_);
        if (isFunCtor_)
            cursor = SerializeVector(cursor, funCtorArgs_);
        return cursor;
    }
};

class ModuleCharsForLookup : ModuleChars
{
    Vector<char16_t, 0, SystemAllocPolicy> chars_;

  public:
    const uint8_t* deserialize(const uint8_t* cursor) {
        uint32_t uncompressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &uncompressedSize);

        uint32_t compressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &compressedSize);

        if (!chars_.resize(uncompressedSize / sizeof(char16_t)))
            return nullptr;

        const char* source = reinterpret_cast<const char*>(cursor);
        char* dest = reinterpret_cast<char*>(chars_.begin());
        if (!LZ4::decompress(source, dest, uncompressedSize))
            return nullptr;

        cursor += compressedSize;

        cursor = ReadScalar<uint32_t>(cursor, &isFunCtor_);
        if (isFunCtor_)
            cursor = DeserializeVector(cursor, &funCtorArgs_);

        return cursor;
    }

    bool match(AsmJSParser& parser) const {
        const char16_t* parseBegin = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char16_t* parseLimit = parser.tokenStream.rawLimit();
        MOZ_ASSERT(parseLimit >= parseBegin);
        if (uint32_t(parseLimit - parseBegin) < chars_.length())
            return false;
        if (!PodEqual(chars_.begin(), parseBegin, chars_.length()))
            return false;
        if (isFunCtor_ != parser.pc->isStandaloneFunctionBody())
            return false;
        if (isFunCtor_) {
            // For function statements, the closing } is included as the last
            // character of the matched source. For Function constructor,
            // parsing terminates with EOF which we must explicitly check. This
            // prevents
            //   new Function('"use asm"; function f() {} return f')
            // from incorrectly matching
            //   new Function('"use asm"; function f() {} return ff')
            if (parseBegin + chars_.length() != parseLimit)
                return false;
            unsigned numArgs;
            FunctionNode* functionNode = parser.pc->functionBox()->functionNode;
            ParseNode* arg = FunctionFormalParametersList(functionNode, &numArgs);
            if (funCtorArgs_.length() != numArgs)
                return false;
            for (unsigned i = 0; i < funCtorArgs_.length(); i++, arg = arg->pn_next) {
                UniqueChars name = StringToNewUTF8CharsZ(nullptr, *arg->name());
                if (!name || strcmp(funCtorArgs_[i].get(), name.get()))
                    return false;
            }
        }
        return true;
    }
};

struct ScopedCacheEntryOpenedForWrite
{
    ExclusiveContext* cx;
    const size_t serializedSize;
    uint8_t* memory;
    intptr_t handle;

    ScopedCacheEntryOpenedForWrite(ExclusiveContext* cx, size_t serializedSize)
      : cx(cx), serializedSize(serializedSize), memory(nullptr), handle(-1)
    {}

    ~ScopedCacheEntryOpenedForWrite() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForWrite(serializedSize, memory, handle);
    }
};

struct ScopedCacheEntryOpenedForRead
{
    ExclusiveContext* cx;
    size_t serializedSize;
    const uint8_t* memory;
    intptr_t handle;

    explicit ScopedCacheEntryOpenedForRead(ExclusiveContext* cx)
      : cx(cx), serializedSize(0), memory(nullptr), handle(0)
    {}

    ~ScopedCacheEntryOpenedForRead() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForRead(serializedSize, memory, handle);
    }
};

} // unnamed namespace

static JS::AsmJSCacheResult
StoreAsmJSModuleInCache(AsmJSParser& parser, Module& module, ExclusiveContext* cx)
{
    ModuleCharsForStore moduleChars;
    if (!moduleChars.init(parser))
        return JS::AsmJSCache_InternalError;

    size_t bytecodeSize, compiledSize;
    module.serializedSize(&bytecodeSize, &compiledSize);
    MOZ_RELEASE_ASSERT(bytecodeSize == 0);
    MOZ_RELEASE_ASSERT(compiledSize <= UINT32_MAX);

    size_t serializedSize = sizeof(uint32_t) +
                            compiledSize +
                            moduleChars.serializedSize();

    JS::OpenAsmJSCacheEntryForWriteOp open = cx->asmJSCacheOps().openEntryForWrite;
    if (!open)
        return JS::AsmJSCache_Disabled_Internal;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* end = parser.tokenStream.rawCharPtrAt(ModuleChars::endOffset(parser));

    ScopedCacheEntryOpenedForWrite entry(cx, serializedSize);
    JS::AsmJSCacheResult openResult =
        open(cx->global(), begin, end, serializedSize, &entry.memory, &entry.handle);
    if (openResult != JS::AsmJSCache_Success)
        return openResult;

    uint8_t* cursor = entry.memory;

    // Everything serialized before the Module must not change incompatibly
    // between any two builds (regardless of platform, architecture, ...).
    // (The Module::assumptionsMatch() guard everything in the Module and
    // afterwards.)
    cursor = WriteScalar<uint32_t>(cursor, compiledSize);

    module.serialize(/* bytecodeBegin = */ nullptr, /* bytecodeSize = */ 0, cursor, compiledSize);
    cursor += compiledSize;

    cursor = moduleChars.serialize(cursor);

    MOZ_RELEASE_ASSERT(cursor == entry.memory + serializedSize);

    return JS::AsmJSCache_Success;
}

static bool
LookupAsmJSModuleInCache(ExclusiveContext* cx, AsmJSParser& parser, bool* loadedFromCache,
                         SharedModule* module, UniqueChars* compilationTimeReport)
{
    int64_t before = PRMJ_Now();

    *loadedFromCache = false;

    JS::OpenAsmJSCacheEntryForReadOp open = cx->asmJSCacheOps().openEntryForRead;
    if (!open)
        return true;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* limit = parser.tokenStream.rawLimit();

    ScopedCacheEntryOpenedForRead entry(cx);
    if (!open(cx->global(), begin, limit, &entry.serializedSize, &entry.memory, &entry.handle))
        return true;

    size_t remain = entry.serializedSize;
    const uint8_t* cursor = entry.memory;

    uint32_t compiledSize;
    cursor = ReadScalarChecked<uint32_t>(cursor, &remain, &compiledSize);
    if (!cursor)
        return true;

    Assumptions assumptions;
    if (!assumptions.initBuildIdFromContext(cx))
        return false;

    if (!Module::assumptionsMatch(assumptions, cursor, remain))
        return true;

    MutableAsmJSMetadata asmJSMetadata = cx->new_<AsmJSMetadata>();
    if (!asmJSMetadata)
        return false;

    *module = Module::deserialize(/* bytecodeBegin = */ nullptr, /* bytecodeSize = */ 0,
                                  cursor, compiledSize, asmJSMetadata.get());
    if (!*module) {
        ReportOutOfMemory(cx);
        return false;
    }
    cursor += compiledSize;

    // Due to the hash comparison made by openEntryForRead, this should succeed
    // with high probability.
    ModuleCharsForLookup moduleChars;
    cursor = moduleChars.deserialize(cursor);
    if (!moduleChars.match(parser))
        return true;

    // Don't punish release users by crashing if there is a programmer error
    // here, just gracefully return with a cache miss.
#ifdef NIGHTLY_BUILD
    MOZ_RELEASE_ASSERT(cursor == entry.memory + entry.serializedSize);
#endif
    if (cursor != entry.memory + entry.serializedSize)
        return true;

    // See AsmJSMetadata comment as well as ModuleValidator::init().
    asmJSMetadata->toStringStart = parser.pc->functionBox()->toStringStart;
    asmJSMetadata->srcStart = parser.pc->functionBox()->functionNode->body()->pn_pos.begin;
    asmJSMetadata->srcBodyStart = parser.tokenStream.currentToken().pos.end;
    asmJSMetadata->strict = parser.pc->sc()->strict() && !parser.pc->sc()->hasExplicitUseStrict();
    asmJSMetadata->scriptSource.reset(parser.ss);

    if (!parser.tokenStream.advance(asmJSMetadata->srcEndBeforeCurly()))
        return false;

    int64_t after = PRMJ_Now();
    int ms = (after - before) / PRMJ_USEC_PER_MSEC;
    *compilationTimeReport = UniqueChars(JS_smprintf("loaded from cache in %dms", ms));
    if (!*compilationTimeReport)
        return false;

    *loadedFromCache = true;
    return true;
}

/*****************************************************************************/
// Top-level js::CompileAsmJS

static bool
NoExceptionPending(ExclusiveContext* cx)
{
    return !cx->isJSContext() || !cx->asJSContext()->isExceptionPending();
}

static bool
Warn(AsmJSParser& parser, int errorNumber, const char* str)
{
    ParseReportKind reportKind = parser.options().throwOnAsmJSValidationFailureOption &&
                                 errorNumber == JSMSG_USE_ASM_TYPE_FAIL
                                 ? ParseError
                                 : ParseWarning;
    parser.reportNoOffset(reportKind, /* strict = */ false, errorNumber, str ? str : "");
    return false;
}

static bool
EstablishPreconditions(ExclusiveContext* cx, AsmJSParser& parser)
{
    if (!HasCompilerSupport(cx))
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by lack of compiler support");

    switch (parser.options().asmJSOption) {
      case AsmJSOption::Disabled:
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by 'asmjs' runtime option");
      case AsmJSOption::DisabledByDebugger:
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by debugger");
      case AsmJSOption::Enabled:
        break;
    }

    if (parser.pc->isStarGenerator() || parser.pc->isLegacyGenerator())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by generator context");

    if (parser.pc->isAsync())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by async context");

    if (parser.pc->isArrowFunction())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by arrow function context");

    // Class constructors are also methods
    if (parser.pc->isMethod())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by class constructor or method context");

    return true;
}

static UniqueChars
BuildConsoleMessage(ExclusiveContext* cx, unsigned time, JS::AsmJSCacheResult cacheResult)
{
#ifndef JS_MORE_DETERMINISTIC
    const char* cacheString = "";
    switch (cacheResult) {
      case JS::AsmJSCache_Success:
        cacheString = "stored in cache";
        break;
      case JS::AsmJSCache_ModuleTooSmall:
        cacheString = "not stored in cache (too small to benefit)";
        break;
      case JS::AsmJSCache_SynchronousScript:
        cacheString = "unable to cache asm.js in synchronous scripts; try loading "
                      "asm.js via <script async> or createElement('script')";
        break;
      case JS::AsmJSCache_QuotaExceeded:
        cacheString = "not enough temporary storage quota to store in cache";
        break;
      case JS::AsmJSCache_StorageInitFailure:
        cacheString = "storage initialization failed (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_Internal:
        cacheString = "caching disabled by internal configuration (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_ShellFlags:
        cacheString = "caching disabled by missing command-line arguments";
        break;
      case JS::AsmJSCache_Disabled_JitInspector:
        cacheString = "caching disabled by active JIT inspector";
        break;
      case JS::AsmJSCache_InternalError:
        cacheString = "unable to store in cache due to internal error (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_PrivateBrowsing:
        cacheString = "caching disabled by private browsing mode";
        break;
      case JS::AsmJSCache_LIMIT:
        MOZ_CRASH("bad AsmJSCacheResult");
        break;
    }

    return UniqueChars(JS_smprintf("total compilation time %dms; %s", time, cacheString));
#else
    return DuplicateString("");
#endif
}

bool
js::CompileAsmJS(ExclusiveContext* cx, AsmJSParser& parser, ParseNode* stmtList, bool* validated)
{
    *validated = false;

    // Various conditions disable asm.js optimizations.
    if (!EstablishPreconditions(cx, parser))
        return NoExceptionPending(cx);

    // Before spending any time parsing the module, try to look it up in the
    // embedding's cache using the chars about to be parsed as the key.
    bool loadedFromCache;
    SharedModule module;
    UniqueChars message;
    if (!LookupAsmJSModuleInCache(cx, parser, &loadedFromCache, &module, &message))
        return false;

    // If not present in the cache, parse, validate and generate code in a
    // single linear pass over the chars of the asm.js module.
    if (!loadedFromCache) {
        // "Checking" parses, validates and compiles, producing a fully compiled
        // WasmModuleObject as result.
        unsigned time;
        module = CheckModule(cx, parser, stmtList, &time);
        if (!module)
            return NoExceptionPending(cx);

        // Try to store the AsmJSModule in the embedding's cache. The
        // AsmJSModule must be stored before static linking since static linking
        // specializes the AsmJSModule to the current process's address space
        // and therefore must be executed after a cache hit.
        JS::AsmJSCacheResult cacheResult = StoreAsmJSModuleInCache(parser, *module, cx);

        // Build the string message to display in the developer console.
        message = BuildConsoleMessage(cx, time, cacheResult);
        if (!message)
            return NoExceptionPending(cx);
    }

    // Hand over ownership to a GC object wrapper which can then be referenced
    // from the module function.
    Rooted<WasmModuleObject*> moduleObj(cx, WasmModuleObject::create(cx, *module));
    if (!moduleObj)
        return false;

    // The module function dynamically links the AsmJSModule when called and
    // generates a set of functions wrapping all the exports.
    FunctionBox* funbox = parser.pc->functionBox();
    RootedFunction moduleFun(cx, NewAsmJSModuleFunction(cx, funbox->function(), moduleObj));
    if (!moduleFun)
        return false;

    // Finished! Clobber the default function created by the parser with the new
    // asm.js module function. Special cases in the bytecode emitter avoid
    // generating bytecode for asm.js functions, allowing this asm.js module
    // function to be the finished result.
    MOZ_ASSERT(funbox->function()->isInterpreted());
    funbox->clobberFunction(moduleFun);

    // Success! Write to the console with a "warning" message.
    *validated = true;
    Warn(parser, JSMSG_USE_ASM_TYPE_OK, message.get());
    return NoExceptionPending(cx);
}

/*****************************************************************************/
// asm.js testing functions

bool
js::IsAsmJSModuleNative(Native native)
{
    return native == InstantiateAsmJS;
}

bool
js::IsAsmJSModule(JSFunction* fun)
{
    return fun->maybeNative() == InstantiateAsmJS;
}

bool
js::IsAsmJSFunction(JSFunction* fun)
{
    if (IsExportedFunction(fun))
        return ExportedFunctionToInstance(fun).metadata().isAsmJS();
    return false;
}

bool
js::IsAsmJSStrictModeModuleOrFunction(JSFunction* fun)
{
    if (IsAsmJSModule(fun))
        return AsmJSModuleFunctionToModule(fun).metadata().asAsmJS().strict;

    if (IsAsmJSFunction(fun))
        return ExportedFunctionToInstance(fun).metadata().asAsmJS().strict;

    return false;
}

bool
js::IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // See EstablishPreconditions.
    bool available = HasCompilerSupport(cx) && cx->options().asmJS();

    args.rval().set(BooleanValue(available));
    return true;
}

static JSFunction*
MaybeWrappedNativeFunction(const Value& v)
{
    if (!v.isObject())
        return nullptr;

    JSObject* obj = CheckedUnwrap(&v.toObject());
    if (!obj)
        return nullptr;

    if (!obj->is<JSFunction>())
        return nullptr;

    return &obj->as<JSFunction>();
}

bool
js::IsAsmJSModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool rval = false;
    if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0)))
        rval = IsAsmJSModule(fun);

    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool rval = false;
    if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0)))
        rval = IsAsmJSFunction(fun);

    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSModuleLoadedFromCache(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunction* fun = MaybeWrappedNativeFunction(args.get(0));
    if (!fun || !IsAsmJSModule(fun)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_USE_ASM_TYPE_FAIL,
                                  "argument passed to isAsmJSModuleLoadedFromCache is not a "
                                  "validated asm.js module");
        return false;
    }

    bool loadedFromCache =
        AsmJSModuleFunctionToModule(fun).metadata().asAsmJS().cacheResult == CacheResult::Hit;

    args.rval().set(BooleanValue(loadedFromCache));
    return true;
}

/*****************************************************************************/
// asm.js toString/toSource support

JSString*
js::AsmJSModuleToString(JSContext* cx, HandleFunction fun, bool isToSource)
{
    MOZ_ASSERT(IsAsmJSModule(fun));

    const AsmJSMetadata& metadata = AsmJSModuleFunctionToModule(fun).metadata().asAsmJS();
    uint32_t begin = metadata.toStringStart;
    uint32_t end = metadata.srcEndAfterCurly();
    ScriptSource* source = metadata.scriptSource.get();

    StringBuffer out(cx);

    if (isToSource && fun->isLambda() && !out.append("("))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        if (!out.append("function "))
            return nullptr;
        if (fun->explicitName() && !out.append(fun->explicitName()))
            return nullptr;
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
        if (!src)
            return nullptr;

        if (!out.append(src))
            return nullptr;
    }

    if (isToSource && fun->isLambda() && !out.append(")"))
        return nullptr;

    return out.finishString();
}

JSString*
js::AsmJSFunctionToString(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(IsAsmJSFunction(fun));

    const AsmJSMetadata& metadata = ExportedFunctionToInstance(fun).metadata().asAsmJS();
    const AsmJSExport& f = metadata.lookupAsmJSExport(ExportedFunctionToFuncIndex(fun));

    uint32_t begin = metadata.srcStart + f.startOffsetInModule();
    uint32_t end = metadata.srcStart + f.endOffsetInModule();

    ScriptSource* source = metadata.scriptSource.get();
    StringBuffer out(cx);

    if (!out.append("function "))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        // asm.js functions can't be anonymous
        MOZ_ASSERT(fun->explicitName());
        if (!out.append(fun->explicitName()))
            return nullptr;
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
        if (!src)
            return nullptr;
        if (!out.append(src))
            return nullptr;
    }

    return out.finishString();
}

bool
js::IsValidAsmJSHeapLength(uint32_t length)
{
    if (length < MinHeapLength)
        return false;

    return wasm::IsValidARMImmediate(length);
}
