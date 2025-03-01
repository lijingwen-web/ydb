#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

void PrepareScheme(NKikimrSchemeOp::TTableDescription* schema, const TString& name, const TTableInfo::TPtr srcTableInfo, TOperationContext &context) {
    const NScheme::TTypeRegistry* typeRegistry = AppData(context.Ctx)->TypeRegistry;

    NKikimrSchemeOp::TTableDescription completedSchema;
    context.SS->DescribeTable(srcTableInfo, typeRegistry, true, false, &completedSchema);
    completedSchema.SetName(name);

    //inherit all from Src exept PartitionConfig, PartitionConfig could be altered
    completedSchema.MutablePartitionConfig()->CopyFrom(schema->GetPartitionConfig());
    schema->Swap(&completedSchema);
}

class TConfigureParts: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TCopyTable TConfigureParts"
                << " operationId#" << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType, });
    }

    bool HandleReply(TEvDataShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                 DebugHint() << " HandleReply TEvProposeTransactionResult"
                 << " at tabletId# " << ssId
                 << " message# " << ev->Get()->Record.ShortDebugString());

        return NTableState::CollectProposeTransactionResults(OperationId, ev, context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     DebugHint() << " ProgressState"
                     << " at tablet" << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        txState->ClearShardsInProgress();

        Y_VERIFY(txState->SourcePathId != InvalidPathId);
        Y_VERIFY(txState->TargetPathId != InvalidPathId);
        const TTableInfo::TPtr srcTableInfo = *context.SS->Tables.FindPtr(txState->SourcePathId);
        const TTableInfo::TPtr dstTableInfo = *context.SS->Tables.FindPtr(txState->TargetPathId);

        Y_VERIFY(srcTableInfo->GetPartitions().size() == dstTableInfo->GetPartitions().size(),
                 "CopyTable paritition counts don't match");
        const ui64 dstSchemaVersion = NEW_TABLE_ALTER_VERSION;

        const ui64 subDomainPathId = context.SS->ResolvePathIdForDomain(txState->TargetPathId).LocalPathId;

        for (ui32 i = 0; i < dstTableInfo->GetPartitions().size(); ++i) {
            TShardIdx srcShardIdx = srcTableInfo->GetPartitions()[i].ShardIdx;
            TTabletId srcDatashardId = context.SS->ShardInfos[srcShardIdx].TabletID;

            TShardIdx dstShardIdx = dstTableInfo->GetPartitions()[i].ShardIdx;
            TTabletId dstDatashardId = context.SS->ShardInfos[dstShardIdx].TabletID;

            auto seqNo = context.SS->StartRound(*txState);

            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                      DebugHint() << " Propose modify scheme on dstDatashard# " << dstDatashardId
                        << " idx# " << dstShardIdx
                        << " srcDatashard# " << srcDatashardId
                        << " idx# " <<  srcShardIdx
                        << " operationId# " << OperationId
                        << " seqNo# " << seqNo
                        << " at tablet# " << ssId);

            // Send "CreateTable + ReceiveParts" transaction to destination datashard
            NKikimrTxDataShard::TFlatSchemeTransaction newShardTx;
            context.SS->FillSeqNo(newShardTx, seqNo);
            context.SS->FillTableDescription(txState->TargetPathId, i, dstSchemaVersion, newShardTx.MutableCreateTable());
            newShardTx.MutableReceiveSnapshot()->SetTableId_Deprecated(txState->TargetPathId.LocalPathId);
            newShardTx.MutableReceiveSnapshot()->MutableTableId()->SetOwnerId(txState->TargetPathId.OwnerId);
            newShardTx.MutableReceiveSnapshot()->MutableTableId()->SetTableId(txState->TargetPathId.LocalPathId);
            newShardTx.MutableReceiveSnapshot()->AddReceiveFrom()->SetShard(ui64(srcDatashardId));
            TString txBody;
            Y_PROTOBUF_SUPPRESS_NODISCARD newShardTx.SerializeToString(&txBody);

            THolder<TEvDataShard::TEvProposeTransaction> dstEvent =
                THolder(new TEvDataShard::TEvProposeTransaction(NKikimrTxDataShard::TX_KIND_SCHEME,
                                                    context.SS->TabletID(),
                                                    subDomainPathId,
                                                    context.Ctx.SelfID,
                                                    ui64(OperationId.GetTxId()),
                                                    txBody,
                                                    context.SS->SelectProcessingPrarams(txState->TargetPathId)));
            context.OnComplete.BindMsgToPipe(OperationId, dstDatashardId, dstShardIdx, dstEvent.Release());

            // Send "SendParts" transaction to source datashard
            NKikimrTxDataShard::TFlatSchemeTransaction oldShardTx;
            context.SS->FillSeqNo(oldShardTx, seqNo);
            oldShardTx.MutableSendSnapshot()->SetTableId_Deprecated(txState->SourcePathId.LocalPathId);
            oldShardTx.MutableSendSnapshot()->MutableTableId()->SetOwnerId(txState->SourcePathId.OwnerId);
            oldShardTx.MutableSendSnapshot()->MutableTableId()->SetTableId(txState->SourcePathId.LocalPathId);
            oldShardTx.MutableSendSnapshot()->AddSendTo()->SetShard(ui64(dstDatashardId));
            oldShardTx.SetReadOnly(true);
            txBody.clear();
            Y_PROTOBUF_SUPPRESS_NODISCARD oldShardTx.SerializeToString(&txBody);
            THolder<TEvDataShard::TEvProposeTransaction> srcEvent =
                THolder(new TEvDataShard::TEvProposeTransaction(NKikimrTxDataShard::TX_KIND_SCHEME,
                                                    context.SS->TabletID(),
                                                    context.Ctx.SelfID,
                                                    ui64(OperationId.GetTxId()),
                                                    txBody,
                                                    context.SS->SelectProcessingPrarams(txState->TargetPathId)));
            context.OnComplete.BindMsgToPipe(OperationId, srcDatashardId, srcShardIdx, srcEvent.Release());
        }

        txState->UpdateShardsInProgress();
        return false;
    }
};


class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TCopyTable TPropose"
                << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvHive::TEvCreateTabletReply::EventType, TEvDataShard::TEvProposeTransactionResult::EventType});
    }

    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvDataShard::TEvSchemaChanged"
                               << " triggers early, save it"
                               << ", at schemeshard: " << ssId);

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << ", stepId: " << step
                               << ", at schemeshard" << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);

        TPathId pathId = txState->TargetPathId;
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);

        NIceDb::TNiceDb db(context.GetDB());

        path->StepCreated = step;
        context.SS->PersistCreateStep(db, pathId, step);

        TTableInfo::TPtr table = context.SS->Tables[pathId];
        Y_VERIFY(table);
        table->AlterVersion = NEW_TABLE_ALTER_VERSION;
        context.SS->PersistTableCreated(db, pathId);

        context.SS->TabletCounters->Simple()[COUNTER_TABLE_COUNT].Add(1);

        if (table->IsTTLEnabled() && !context.SS->TTLEnabledTables.contains(pathId)) {
            context.SS->TTLEnabledTables[pathId] = table;
            context.SS->TabletCounters->Simple()[COUNTER_TTL_ENABLED_TABLE_COUNT].Add(1);

            const auto now = context.Ctx.Now();
            for (auto& shard : table->GetPartitions()) {
                auto& lag = shard.LastCondEraseLag;
                Y_VERIFY_DEBUG(!lag.Defined());

                lag = now - shard.LastCondErase;
                context.SS->TabletCounters->Percentile()[COUNTER_NUM_SHARDS_BY_TTL_LAG].IncrementFor(lag->Seconds());
            }
        }

        auto parentDir = context.SS->PathsById.at(path->ParentPathId); // TargetPathId has been created
        ++parentDir->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentDir);
        context.SS->ClearDescribePathCaches(parentDir);
        context.OnComplete.PublishToSchemeBoard(OperationId, parentDir->PathId);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        TSet<TTabletId> shardSet;
        for (const auto& shard : txState->Shards) {
            TShardIdx idx = shard.Idx;
            TTabletId tablet = context.SS->ShardInfos.at(idx).TabletID;
            shardSet.insert(tablet);
        }

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, shardSet);
        return false;
    }
};


class TCopyTable: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::CreateParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return TTxState::ConfigureParts;
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::ProposedWaitParts;
        case TTxState::ProposedWaitParts:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
        return TTxState::Invalid;
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return THolder(new TCreateParts(OperationId));
        case TTxState::ConfigureParts:
            return THolder(new TConfigureParts(OperationId));
        case TTxState::Propose:
            return THolder(new TPropose(OperationId));
        case TTxState::ProposedWaitParts:
            return THolder(new NTableState::TProposedWaitParts(OperationId));
        case TTxState::Done:
            return THolder(new TDone(OperationId));
        default:
            return nullptr;
        }
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

public:
    TCopyTable(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCopyTable(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    bool IsShadowDataAllowed() const {
        return AppData()->AllowShadowDataInSchemeShardForTests;
    }


    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const TString& parentPath = Transaction.GetWorkingDir();
        const TString& name = Transaction.GetCreateTable().GetName();
        const auto acceptExisted = !Transaction.GetFailOnExist();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCopyTable Propose"
                         << ", path: " << parentPath << "/" << name
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));

        TPath parent = TPath::Resolve(parentPath, context.SS);
        {
            TPath::TChecker checks = parent.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting();

            if (checks) {
                if (parent.Base()->IsTableIndex()) {
                    checks
                        .IsInsideTableIndexPath()
                        .IsUnderCreating(NKikimrScheme::StatusNameConflict)
                        .IsUnderTheSameOperation(OperationId.GetTxId()); //allow only as part of copying base table
                } else {
                    checks
                        .NotUnderOperation()
                        .IsCommonSensePath()
                        .IsLikeDirectory();
                }
            }

            if (!checks) {
                TString explain = TStringBuilder() << "parent path fail checks"
                                                   << ", path: " << parent.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        TPath srcPath = TPath::Resolve(Transaction.GetCreateTable().GetCopyFromTable(), context.SS);
        {
            TPath::TChecker checks = srcPath.Check();
            checks
                .NotEmpty()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsTable()
                .NotUnderTheSameOperation(OperationId.GetTxId())
                .NotUnderOperation();

            if (checks) {
                if (parent.Base()->IsTableIndex()) {
                    checks
                        .IsInsideTableIndexPath() //copy imp index table as index index table, not a separate one
                        .NotChildren(); //imp table doesnt have indexes
                } else {
                    checks.IsCommonSensePath();
                }
            }

            if (!checks) {
                TString explain = TStringBuilder() << "src path fail checks"
                                                   << ", path: " << srcPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        const ui32 maxShardsToCreate = srcPath.Shards();
        const TString acl = Transaction.GetModifyACL().GetDiffACL();

        auto schema = Transaction.GetCreateTable();
        const bool isBackup = schema.GetIsBackup();

        TPath dstPath = parent.Child(name);
        {
            TPath::TChecker checks = dstPath.Check();
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(TPathElement::EPathType::EPathTypeTable, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                if (!parent.Base()->IsTableIndex()) {
                    checks.DepthLimit();
                }

                checks
                    .IsValidLeafName()
                    .IsTheSameDomain(srcPath)
                    .PathShardsLimit(maxShardsToCreate)
                    .IsValidACL(acl);
            }

            if (checks && !isBackup) {
                checks
                    .PathsLimit()
                    .DirChildrenLimit()
                    .ShardsLimit(maxShardsToCreate);
            }

            if (!checks) {
                TString explain = TStringBuilder() << "path fail checks"
                                                   << ", path: " << dstPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        auto domainInfo = parent.DomainInfo();
        bool transactionSupport = domainInfo->IsSupportTransactions();
        if (domainInfo->GetAlter()) {
            TPathId domainPathId = parent.GetPathIdForDomain();
            Y_VERIFY(context.SS->PathsById.contains(domainPathId));
            TPathElement::TPtr domainPath = context.SS->PathsById.at(domainPathId);
            Y_VERIFY(domainPath->PlannedToCreate() || domainPath->HasActiveChanges());

            transactionSupport |= domainInfo->GetAlter()->IsSupportTransactions();
        }
        if (!transactionSupport) {
            result->SetError(NKikimrScheme::StatusNameConflict, "Inclusive subDomian do not support shared transactions");
            return result;
        }

        TString errStr;

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!context.SS->CheckLocks(srcPath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        Y_VERIFY(context.SS->Tables.contains(srcPath.Base()->PathId));
        TTableInfo::TPtr srcTableInfo = context.SS->Tables.at(srcPath.Base()->PathId);

        // do not allow copy from table with enabled external blobs
        {
            const NKikimrSchemeOp::TPartitionConfig &srcPartitionConfig = srcTableInfo->PartitionConfig();
            if (PartitionConfigHasExternalBlobsEnabled(srcPartitionConfig)) {
                result->SetError(NKikimrScheme::StatusPreconditionFailed, "source table contains external blobs, copy operation is not safe so prohibited");
                return result;
            }
            if (srcPartitionConfig.GetShadowData()) {
                result->SetError(NKikimrScheme::StatusPreconditionFailed, "Cannot copy tables with enabled ShadowData");
                return result;
            }
        }

        const bool omitFollowers = schema.GetOmitFollowers();

        PrepareScheme(&schema, name, srcTableInfo, context);
        schema.SetIsBackup(isBackup);

        if (omitFollowers) {
            schema.MutablePartitionConfig()->AddFollowerGroups()->Clear();
        }

        if (isBackup) {
            schema.ClearTTLSettings();
        }

        NKikimrSchemeOp::TPartitionConfig compilationPartitionConfig;
        if (!TPartitionConfigMerger::ApplyChanges(compilationPartitionConfig, srcTableInfo->PartitionConfig(), schema.GetPartitionConfig(), AppData(), errStr)
            || !TPartitionConfigMerger::VerifyCreateParams(compilationPartitionConfig, AppData(), IsShadowDataAllowed(), errStr)) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
            return result;
        }
        schema.MutablePartitionConfig()->CopyFrom(compilationPartitionConfig);

        const NScheme::TTypeRegistry* typeRegistry = AppData()->TypeRegistry;
        const TSchemeLimits& limits = domainInfo->GetSchemeLimits();
        TTableInfo::TAlterDataPtr alterData = TTableInfo::CreateAlterData(nullptr, schema, *typeRegistry, limits, *domainInfo, errStr);
        if (!alterData.Get()) {
            result->SetError(NKikimrScheme::StatusSchemeError, errStr);
            return result;
        }

        TTableInfo::TPtr tableInfo = new TTableInfo(std::move(*alterData));
        alterData.Reset();

        TChannelsBindings channelsBinding;

        bool storePerShardConfig = false;
        NKikimrSchemeOp::TPartitionConfig perShardConfig;

        if (context.SS->IsStorageConfigLogic(tableInfo)) {
            TVector<TStorageRoom> storageRooms;
            THashMap<ui32, ui32> familyRooms;
            storageRooms.emplace_back(0);

            if (!context.SS->GetBindingsRooms(dstPath.GetPathIdForDomain(), tableInfo->PartitionConfig(), storageRooms, familyRooms, channelsBinding, errStr)) {
                errStr = TString("database doesn't have required storage pools to create tablet with storage config, details: ") + errStr;
                result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
                return result;
            }
            tableInfo->SetRoom(storageRooms[0]);

            storePerShardConfig = true;
            for (const auto& room : storageRooms) {
                perShardConfig.AddStorageRooms()->CopyFrom(room);
            }
            for (const auto& familyRoom : familyRooms) {
                auto* protoFamily = perShardConfig.AddColumnFamilies();
                protoFamily->SetId(familyRoom.first);
                protoFamily->SetRoom(familyRoom.second);
            }
        } else if (context.SS->IsCompatibleChannelProfileLogic(dstPath.GetPathIdForDomain(), tableInfo)) {
            if (!context.SS->GetChannelsBindings(dstPath.GetPathIdForDomain(), tableInfo, channelsBinding, errStr)) {
                result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
                return result;
            }
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCopyTable, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        auto guard = context.DbGuard();
        TPathId allocatedPathId = context.SS->AllocatePathId();
        context.MemChanges.GrabNewPath(context.SS, allocatedPathId);
        context.MemChanges.GrabPath(context.SS, parent.Base()->PathId);
        context.MemChanges.GrabPath(context.SS, srcPath.Base()->PathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);
        context.MemChanges.GrabDomain(context.SS, parent.GetPathIdForDomain());
        context.MemChanges.GrabNewTable(context.SS, allocatedPathId);

        context.DbChanges.PersistPath(allocatedPathId);
        context.DbChanges.PersistPath(parent.Base()->PathId);
        context.DbChanges.PersistPath(srcPath.Base()->PathId);
        context.DbChanges.PersistApplyUserAttrs(allocatedPathId);
        context.DbChanges.PersistTable(allocatedPathId);
        context.DbChanges.PersistTxState(OperationId);

        // copy attrs without any checks
        TUserAttributes::TPtr userAttrs = new TUserAttributes(1);
        userAttrs->Attrs = srcPath.Base()->UserAttrs->Attrs;

        dstPath.MaterializeLeaf(owner, allocatedPathId);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        TPathElement::TPtr newTable = dstPath.Base();
        newTable->CreateTxId = OperationId.GetTxId();
        newTable->LastTxId = OperationId.GetTxId();
        newTable->PathState = TPathElement::EPathState::EPathStateCreate;
        newTable->PathType = TPathElement::EPathType::EPathTypeTable;
        newTable->UserAttrs->AlterData = userAttrs;

        srcPath.Base()->PathState = TPathElement::EPathState::EPathStateCopying;
        srcPath.Base()->LastTxId = OperationId.GetTxId();

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCopyTable, newTable->PathId, srcPath.Base()->PathId);
        txState.State = TTxState::CreateParts;

        TShardInfo datashardInfo = TShardInfo::DataShardInfo(OperationId.GetTxId(), newTable->PathId);
        datashardInfo.BindedChannels = channelsBinding;
        auto newPartition = NTableState::ApplyPartitioningCopyTable(datashardInfo, srcTableInfo, txState, context.SS);
        for (const auto& part: newPartition) {
            context.MemChanges.GrabNewShard(context.SS, part.ShardIdx);
            context.DbChanges.PersistShard(part.ShardIdx);
        }
        context.SS->SetPartitioning(newTable->PathId, tableInfo, std::move(newPartition));
        for (const auto& shard : tableInfo->GetPartitions()) {
            Y_VERIFY(context.SS->ShardInfos.contains(shard.ShardIdx), "shard info is set before");
            if (storePerShardConfig) {
                tableInfo->PerShardPartitionConfig[shard.ShardIdx].CopyFrom(perShardConfig);
            }
        }

        Y_VERIFY(tableInfo->GetPartitions().back().EndOfRange.empty(), "End of last range must be +INF");

        context.SS->Tables[newTable->PathId] = tableInfo;
        context.SS->IncrementPathDbRefCount(newTable->PathId);

        if (parent.Base()->HasActiveChanges()) {
            TTxId parentTxId = parent.Base()->PlannedToCreate() ? parent.Base()->CreateTxId : parent.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        // Add dependencies on in-flight split operations for source table in case of CopyTable
        Y_VERIFY(txState.SourcePathId != InvalidPathId);
        for (auto splitTx: context.SS->Tables.at(srcPath.Base()->PathId)->GetSplitOpsInFlight()) {
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                         "TCopyTable Propose "
                            << " opId: " << OperationId
                            << " wait split ops in flight on src table " << splitTx);
            context.OnComplete.Dependence(splitTx.GetTxId(), OperationId.GetTxId());
        }

        context.OnComplete.ActivateTx(OperationId);

        ++parent.Base()->DirAlterVersion;
        context.SS->ClearDescribePathCaches(parent.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, parent.Base()->PathId);

        context.SS->ClearDescribePathCaches(newTable);
        context.OnComplete.PublishToSchemeBoard(OperationId, newTable->PathId);

        const ui32 shardsToCreate = tableInfo->GetPartitions().size();
        Y_VERIFY_S(shardsToCreate <= maxShardsToCreate, "shardsToCreate: " << shardsToCreate << " maxShardsToCreate: " << maxShardsToCreate);

        dstPath.DomainInfo()->IncPathsInside(1, isBackup);
        dstPath.DomainInfo()->AddInternalShards(txState, isBackup);
        dstPath.Base()->IncShardsInside(shardsToCreate);
        parent.Base()->IncAliveChildren(1, isBackup);

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCopyTable AbortPropose"
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << context.SS->TabletID());
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCopyTable AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        //txState->TargetPathId should be marked as drop by forceDropTxId

        TPathId srcPathId = txState->SourcePathId;
        Y_VERIFY(srcPathId != InvalidPathId);
        Y_VERIFY(context.SS->PathsById.contains(srcPathId));
        TPathElement::TPtr srcPath = context.SS->PathsById.at(srcPathId);
        srcPath->PathState = TPathElement::EPathState::EPathStateDrop;

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateCopyTable(TOperationId id, const TTxTransaction& tx) {
    return new TCopyTable(id, tx);
}

ISubOperationBase::TPtr CreateCopyTable(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCopyTable(id, state);
}

TVector<ISubOperationBase::TPtr> CreateCopyTable(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_VERIFY(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable);

    auto copying = tx.GetCreateTable();
    Y_VERIFY(copying.HasCopyFromTable());

    TPath srcPath = TPath::Resolve(copying.GetCopyFromTable(), context.SS);
    {
        TPath::TChecker checks = srcPath.Check();
        checks.NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .NotUnderDeleting()
            .IsTable()
            .NotUnderOperation()
            .IsCommonSensePath(); //forbid copy impl index tables directly

        if (!checks) {
            TString explain = TStringBuilder() << "src path fail checks"
                                               << ", path: " << srcPath.PathString();
            auto status = checks.GetStatus(&explain);
            return {CreateReject(nextId, status, explain)};
        }
    }

    TPath workDir = TPath::Resolve(tx.GetWorkingDir(), context.SS);
    TPath dstPath = workDir.Child(copying.GetName());

    TVector<ISubOperationBase::TPtr> result;
    {
        auto schema = TransactionTemplate(tx.GetWorkingDir(), NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable);
        schema.SetFailOnExist(tx.GetFailOnExist());

        auto operation = schema.MutableCreateTable();
        operation->SetName(copying.GetName());
        operation->SetCopyFromTable(copying.GetCopyFromTable());
        operation->SetOmitFollowers(copying.GetOmitFollowers());
        operation->SetIsBackup(copying.GetIsBackup());
        operation->MutablePartitionConfig()->CopyFrom(copying.GetPartitionConfig());

        result.push_back(CreateCopyTable(NextPartId(nextId, result), schema));
    }

    for (auto& child: srcPath.Base()->GetChildren()) {
        auto name = child.first;
        auto pathId = child.second;

        TPath childPath = srcPath.Child(name);
        if (!childPath.IsTableIndex() || childPath.IsDeleted()) {
            continue;
        }

        Y_VERIFY(childPath.Base()->PathId == pathId);
        Y_VERIFY_S(childPath.Base()->GetChildren().size() == 1, childPath.PathString() << " has children " << childPath.Base()->GetChildren().size());

        TTableIndexInfo::TPtr indexInfo = context.SS->Indexes.at(pathId);
        {
            auto schema = TransactionTemplate(dstPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpCreateTableIndex);
            schema.SetFailOnExist(tx.GetFailOnExist());

            auto operation = schema.MutableCreateTableIndex();
            operation->SetName(name);
            operation->SetType(indexInfo->Type);
            for (const auto& keyName: indexInfo->IndexKeys) {
                *operation->MutableKeyColumnNames()->Add() = keyName;
            }
            for (const auto& dataColumn: indexInfo->IndexDataColumns) {
                *operation->MutableDataColumnNames()->Add() = dataColumn;
            }

            result.push_back(CreateNewTableIndex(NextPartId(nextId, result), schema));
        }

        TString implTableName = childPath.Base()->GetChildren().begin()->first;
        TPath implTable = childPath.Child(implTableName);
        Y_VERIFY(implTable.Base()->PathId == childPath.Base()->GetChildren().begin()->second);

        {
            NKikimrSchemeOp::TModifyScheme schema;
            schema.SetFailOnExist(tx.GetFailOnExist());
            schema.SetWorkingDir(JoinPath({dstPath.PathString(), name}));
            schema.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable);

            auto operation = schema.MutableCreateTable();
            operation->SetName(implTableName);
            operation->SetCopyFromTable(implTable.PathString());
            operation->SetOmitFollowers(copying.GetOmitFollowers());
            operation->SetIsBackup(copying.GetIsBackup());

            result.push_back(CreateCopyTable(NextPartId(nextId, result), schema));
        }
    }

    return result;
}

}
}
