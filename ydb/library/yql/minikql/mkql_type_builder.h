#pragma once

#include "mkql_node.h"

#include <ydb/library/yql/public/udf/udf_type_builder.h>

namespace NKikimr {
namespace NMiniKQL {

//////////////////////////////////////////////////////////////////////////////
// TFunctionTypeInfo
//////////////////////////////////////////////////////////////////////////////
struct TFunctionTypeInfo
{
    TCallableType* FunctionType = nullptr;
    const TType* RunConfigType = nullptr;
    const TType* UserType = nullptr;
    NUdf::TUniquePtr<NUdf::IBoxedValue> Implementation;
    NUdf::TUniquePtr<NUdf::IBoxedValue> BlockImplementation;
    TString ModuleIR;
    TString ModuleIRUniqID;
    TString IRFunctionName;
    bool Deterministic = true;
};

//////////////////////////////////////////////////////////////////////////////
// TArgInfo
//////////////////////////////////////////////////////////////////////////////
struct TArgInfo {
    NMiniKQL::TType* Type_ = nullptr;
    TInternName Name_;
    ui64 Flags_;
};

//////////////////////////////////////////////////////////////////////////////
// TFunctionTypeInfoBuilder
//////////////////////////////////////////////////////////////////////////////
class TFunctionTypeInfoBuilder: public NUdf::IFunctionTypeInfoBuilder
{
public:
    TFunctionTypeInfoBuilder(
            const TTypeEnvironment& env,
            NUdf::ITypeInfoHelper::TPtr typeInfoHelper,
            const TStringBuf& moduleName,
            NUdf::ICountersProvider* countersProvider,
            const NUdf::TSourcePosition& pos,
            const NUdf::ISecureParamsProvider* provider = nullptr);

    NUdf::IFunctionTypeInfoBuilder1& ImplementationImpl(
            NUdf::TUniquePtr<NUdf::IBoxedValue> impl) override;

    NUdf::IFunctionTypeInfoBuilder1& ReturnsImpl(NUdf::TDataTypeId typeId) override;
    NUdf::IFunctionTypeInfoBuilder1& ReturnsImpl(const NUdf::TType* type) override;
    NUdf::IFunctionTypeInfoBuilder1& ReturnsImpl(
            const NUdf::ITypeBuilder& typeBuilder) override;

    NUdf::IFunctionArgTypesBuilder::TPtr Args(ui32 expectedItem) override;
    NUdf::IFunctionTypeInfoBuilder1& OptionalArgsImpl(ui32 optionalArgs) override;
    NUdf::IFunctionTypeInfoBuilder1& PayloadImpl(const NUdf::TStringRef& payload) override;

    NUdf::IFunctionTypeInfoBuilder1& RunConfigImpl(NUdf::TDataTypeId typeId) override;
    NUdf::IFunctionTypeInfoBuilder1& RunConfigImpl(const NUdf::TType* type) override;
    NUdf::IFunctionTypeInfoBuilder1& RunConfigImpl(
            const NUdf::ITypeBuilder& typeBuilder) override;

    NUdf::IFunctionTypeInfoBuilder1& UserTypeImpl(NUdf::TDataTypeId typeId) override;
    NUdf::IFunctionTypeInfoBuilder1& UserTypeImpl(const NUdf::TType* type) override;
    NUdf::IFunctionTypeInfoBuilder1& UserTypeImpl(const NUdf::ITypeBuilder& typeBuilder) override;

    void SetError(const NUdf::TStringRef& error) override;
    inline bool HasError() const { return !Error_.empty(); }
    inline const TString& GetError() const { return Error_; }

    void Build(TFunctionTypeInfo* funcInfo);

    NUdf::TType* Primitive(NUdf::TDataTypeId typeId) const override;
    NUdf::IOptionalTypeBuilder::TPtr Optional() const override;
    NUdf::IListTypeBuilder::TPtr List() const override;
    NUdf::IDictTypeBuilder::TPtr Dict() const override;
    NUdf::IStructTypeBuilder::TPtr Struct(ui32 expectedItems) const override;
    NUdf::ITupleTypeBuilder::TPtr Tuple(ui32 expectedItems) const override;
    NUdf::ICallableTypeBuilder::TPtr Callable(ui32 expectedArgs) const override;

    NUdf::TType* Void() const override;
    NUdf::TType* Resource(const NUdf::TStringRef& tag) const override;
    NUdf::IVariantTypeBuilder::TPtr Variant() const override;

    NUdf::IStreamTypeBuilder::TPtr Stream() const override;

    NUdf::ITypeInfoHelper::TPtr TypeInfoHelper() const override;

    const TTypeEnvironment& Env() const { return Env_; }

    NUdf::TCounter GetCounter(const NUdf::TStringRef& name, bool deriv) override;
    NUdf::TScopedProbe GetScopedProbe(const NUdf::TStringRef& name) override;
    NUdf::TSourcePosition GetSourcePosition() override;

    NUdf::IHash::TPtr MakeHash(const NUdf::TType* type) override;
    NUdf::IEquate::TPtr MakeEquate(const NUdf::TType* type) override;
    NUdf::ICompare::TPtr MakeCompare(const NUdf::TType* type) override;

    NUdf::TType* Decimal(ui8 precision, ui8 scale) const override;

    NUdf::IFunctionTypeInfoBuilder7& IRImplementationImpl(
        const NUdf::TStringRef& moduleIR,
        const NUdf::TStringRef& moduleIRUniqId,
        const NUdf::TStringRef& functionName
    ) override;

    NUdf::TType* Null() const override;
    NUdf::TType* EmptyList() const override;
    NUdf::TType* EmptyDict() const override;

    NUdf::IFunctionTypeInfoBuilder9& BlockImplementationImpl(
        NUdf::TUniquePtr<NUdf::IBoxedValue> impl) override;

    NUdf::ISetTypeBuilder::TPtr Set() const override;
    NUdf::IEnumTypeBuilder::TPtr Enum(ui32 expectedItems = 10) const override;
    NUdf::TType* Tagged(const NUdf::TType* baseType, const NUdf::TStringRef& tag) const override;
    NUdf::TType* Pg(ui32 typeId) const override;
    bool GetSecureParam(NUdf::TStringRef key, NUdf::TStringRef& value) const override;

private:
    const TTypeEnvironment& Env_;
    NUdf::TUniquePtr<NUdf::IBoxedValue> Implementation_;
    NUdf::TUniquePtr<NUdf::IBoxedValue> BlockImplementation_;
    const TType* ReturnType_;
    const TType* RunConfigType_;
    const TType* UserType_;
    TVector<TArgInfo> Args_;
    TString Error_;
    ui32 OptionalArgs_ = 0;
    TString Payload_;
    NUdf::ITypeInfoHelper::TPtr TypeInfoHelper_;
    TStringBuf ModuleName_;
    NUdf::ICountersProvider* CountersProvider_;
    NUdf::TSourcePosition Pos_;
    const NUdf::ISecureParamsProvider* SecureParamsProvider_;
    TString ModuleIR_;
    TString ModuleIRUniqID_;
    TString IRFunctionName_;
};

class TTypeInfoHelper : public NUdf::ITypeInfoHelper
{
public:
    NUdf::ETypeKind GetTypeKind(const NUdf::TType* type) const override;
    void VisitType(const NUdf::TType* type, NUdf::ITypeVisitor* visitor) const override;
    bool IsSameType(const NUdf::TType* type1, const NUdf::TType* type2) const override;
    const NYql::NUdf::TPgTypeDescription* FindPgTypeDescription(ui32 typeId) const override;

private:
    static void DoData(const NMiniKQL::TDataType* dt, NUdf::ITypeVisitor* v);
    static void DoStruct(const NMiniKQL::TStructType* st, NUdf::ITypeVisitor* v);
    static void DoList(const NMiniKQL::TListType* lt, NUdf::ITypeVisitor* v);
    static void DoOptional(const NMiniKQL::TOptionalType* lt, NUdf::ITypeVisitor* v);
    static void DoTuple(const NMiniKQL::TTupleType* tt, NUdf::ITypeVisitor* v);
    static void DoDict(const NMiniKQL::TDictType* dt, NUdf::ITypeVisitor* v);
    static void DoCallable(const NMiniKQL::TCallableType* ct, NUdf::ITypeVisitor* v);
    static void DoVariant(const NMiniKQL::TVariantType* vt, NUdf::ITypeVisitor* v);
    static void DoStream(const NMiniKQL::TStreamType* st, NUdf::ITypeVisitor* v);
    static void DoResource(const NMiniKQL::TResourceType* rt, NUdf::ITypeVisitor* v);
    static void DoTagged(const NMiniKQL::TTaggedType* tt, NUdf::ITypeVisitor* v);
    static void DoPg(const NMiniKQL::TPgType* tt, NUdf::ITypeVisitor* v);
};

NUdf::IHash::TPtr MakeHashImpl(const NMiniKQL::TType* type);
NUdf::ICompare::TPtr MakeCompareImpl(const NMiniKQL::TType* type);
NUdf::IEquate::TPtr MakeEquateImpl(const NMiniKQL::TType* type);

NUdf::IHash::TPtr MakePgHash(const NMiniKQL::TPgType* type);
NUdf::ICompare::TPtr MakePgCompare(const NMiniKQL::TPgType* type);
NUdf::IEquate::TPtr MakePgEquate(const NMiniKQL::TPgType* type);

} // namespace NMiniKQL
} // namespace Nkikimr
