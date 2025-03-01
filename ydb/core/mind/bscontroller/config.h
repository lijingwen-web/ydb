#pragma once

#include "defs.h"
#include "error.h"
#include "impl.h"

namespace NKikimr {
    namespace NBsController {

        class TBlobStorageController::TConfigState {
            template<typename T>
            class TCowHolder {
                T *Origin;
                THolder<T> Ptr;
                std::function<THolder<T> (T*)> Clone;

            public:
                TCowHolder(T *ptr, std::function<THolder<T> (T*)> clone = {})
                    : Origin(ptr)
                    , Clone(std::move(clone))
                {}

                const T &Get() const {
                    return *(Ptr ? Ptr.Get() : Origin);
                }

                T &Unshare() {
                    if (!Ptr) {
                        Ptr = Clone ? Clone(Origin) : MakeHolder<T>(*Origin);
                    }
                    return *Ptr;
                }

                bool Changed() const {
                    return Ptr != nullptr;
                }

                void Commit() {
                    if (Ptr) {
                        std::swap(*Origin, *Ptr);
                    }
                }
            };

        public:
            TBlobStorageController& Self;

            // user-defined configuration
            TCowHolder<TMap<THostConfigId, THostConfigInfo>> HostConfigs;
            TCowHolder<TMap<TBoxId, TBoxInfo>> Boxes;
            TCowHolder<TMap<TBoxStoragePoolId, TStoragePoolInfo>> StoragePools;
            TCowHolder<TMultiMap<TBoxStoragePoolId, TGroupId>> StoragePoolGroups;

            // system-level configuration
            TOverlayMap<TPDiskId, TPDiskInfo> PDisks;
            TOverlayMap<TSerial, TDriveSerialInfo> DrivesSerials;
            TCowHolder<TMap<TNodeId, TNodeInfo>> Nodes;
            TOverlayMap<TVSlotId, TVSlotInfo> VSlots;
            TOverlayMap<TGroupId, TGroupInfo> Groups;
            TCowHolder<TMap<TGroupSpecies, TVector<TGroupId>>> IndexGroupSpeciesToGroup;
            TCowHolder<Schema::Group::ID::Type> NextGroupId;
            TCowHolder<Schema::Group::ID::Type> NextVirtualGroupId;
            TCowHolder<Schema::State::NextStoragePoolId::Type> NextStoragePoolId;

            // helper classes
            using TLocationMap = THashMap<TPDiskLocation, TPDiskId>;
            TLocationMap PDiskLocationMap;
            TLocationMap StaticPDiskLocationMap;

            THostRecordMap HostRecords;

            ui64 UniqueId = RandomNumber<ui64>();

            // volatile reconfiguration state
            THashMap<TVSlotId, TPDiskId> ExplicitReconfigureMap;
            std::set<TVSlotId> SuppressDonorMode;

            // just-created vslots, which are not yet committed to the storage
            TSet<TVSlotId> UncommittedVSlots;

            // PDisks we are going to delete
            THashSet<TPDiskId> PDisksToRemove;

            // outgoing messages
            std::deque<std::unique_ptr<IEventHandle>> Outbox;

            // deferred callbacks
            std::deque<std::function<void()>> Callbacks;

            // when the config cmd received
            const TInstant Timestamp;

            // various settings from controller
            const bool DonorMode;

            // default number for ExpectedSlotCount
            const ui32 DefaultMaxSlots;

            // static pdisk/vdisk states
            std::map<TVSlotId, TStaticVSlotInfo>& StaticVSlots;
            std::map<TPDiskId, TStaticPDiskInfo>& StaticPDisks;
            const std::map<TString, TNodeId>& NodeForSerial;

            TCowHolder<Schema::State::SerialManagementStage::Type> SerialManagementStage;

            TStoragePoolStat& StoragePoolStat;

        public:
            TConfigState(TBlobStorageController &controller, const THostRecordMap &hostRecords, TInstant timestamp)
                : Self(controller)
                , HostConfigs(&controller.HostConfigs)
                , Boxes(&controller.Boxes)
                , StoragePools(&controller.StoragePools)
                , StoragePoolGroups(&controller.StoragePoolGroups)
                , PDisks(controller.PDisks)
                , DrivesSerials(controller.DrivesSerials)
                , Nodes(&controller.Nodes)
                , VSlots(controller.VSlots)
                , Groups(controller.GroupMap)
                , IndexGroupSpeciesToGroup(&controller.IndexGroupSpeciesToGroup)
                , NextGroupId(&controller.NextGroupID)
                , NextVirtualGroupId(&controller.NextVirtualGroupId)
                , NextStoragePoolId(&controller.NextStoragePoolId)
                , HostRecords(hostRecords)
                , Timestamp(timestamp)
                , DonorMode(controller.DonorMode)
                , DefaultMaxSlots(controller.DefaultMaxSlots)
                , StaticVSlots(controller.StaticVSlots)
                , StaticPDisks(controller.StaticPDisks)
                , NodeForSerial(controller.NodeForSerial)
                , SerialManagementStage(&controller.SerialManagementStage)
                , StoragePoolStat(*controller.StoragePoolStat)
            {
                Y_VERIFY(HostRecords);
                Init();
            }

            void Commit() {
                HostConfigs.Commit();
                Boxes.Commit();
                StoragePools.Commit();
                StoragePoolGroups.Commit();
                PDisks.Commit();
                DrivesSerials.Commit();
                Nodes.Commit();
                VSlots.Commit();
                Groups.Commit();
                IndexGroupSpeciesToGroup.Commit();
                NextGroupId.Commit();
                NextVirtualGroupId.Commit();
                NextStoragePoolId.Commit();
                SerialManagementStage.Commit();
            }

            void Rollback() {
                PDisks.Rollback();
                VSlots.Rollback();
                Groups.Rollback();
            }

            bool Changed() const {
                return HostConfigs.Changed() || Boxes.Changed() || StoragePools.Changed() ||
                    StoragePoolGroups.Changed() || PDisks.Changed() || DrivesSerials.Changed() || Nodes.Changed() ||
                    VSlots.Changed() || Groups.Changed() || IndexGroupSpeciesToGroup.Changed() || NextGroupId.Changed() ||
                    NextStoragePoolId.Changed() || SerialManagementStage.Changed() || NextVirtualGroupId.Changed();
            }

            bool NormalizeHostKey(NKikimrBlobStorage::THostKey *host) const {
                if (!host->GetNodeId()) {
                    const THostId key(host->GetFqdn(), host->GetIcPort());
                    if (const auto& nodeId = HostRecords->ResolveNodeId(key)) {
                        host->SetNodeId(*nodeId);
                        return true;
                    } else {
                        return false;
                    }
                }

                if (!host->GetFqdn() && !host->GetIcPort()) {
                    if (const auto& hostId = HostRecords->GetHostId(host->GetNodeId())) {
                        host->SetFqdn(std::get<0>(*hostId));
                        host->SetIcPort(std::get<1>(*hostId));
                        return true;
                    } else {
                        return false;
                    }
                }

                // when both fqdn:port and node id were filled, we have to ensure that they match each other
                return HostRecords->GetHostId(host->GetNodeId()) == std::make_tuple(host->GetFqdn(), host->GetIcPort());
            }

            NKikimrBlobStorage::THostKey NormalizeHostKey(const NKikimrBlobStorage::THostKey& host) const {
                NKikimrBlobStorage::THostKey res(host);
                if (!NormalizeHostKey(&res)) {
                    throw TExHostNotFound(host) << "HostKey# " << host.DebugString() << " incorrect";
                }
                return res;
            }

            void DestroyVSlot(const TVSlotId vslotId);

            void CheckConsistency() const;

            void ApplyConfigUpdates();

        private:
            void Init() {
                PDisks.ForEach([this](const TPDiskId& pdiskId, const TPDiskInfo& pdiskInfo) {
                    const TNodeId &nodeId = pdiskId.NodeId;
                    TPDiskLocation location{nodeId, pdiskInfo.PathOrSerial()};
                    PDiskLocationMap.emplace(location, pdiskId);
                });
                for (const auto& [pdiskId, pdisk] : StaticPDisks) {
                    TPDiskLocation location{pdiskId.NodeId, pdisk.Path};
                    StaticPDiskLocationMap.emplace(location, pdiskId);
                }
            }

        private:
            template<typename TCommand, typename TKey, typename TValue>
            static ui64 CheckGeneration(const TCommand &cmd, const TMap<TKey, TValue> &map, const TKey &id) {
                const ui64 cmdGen = cmd.GetItemConfigGeneration();
                const auto it = map.find(id);
                const ui64 itemGen = it != map.end() ? it->second.Generation.GetOrElse(1) : 0;
                if (cmdGen != Max<ui64>() && cmdGen != itemGen) {
                    throw TExItemConfigGenerationMismatch(cmdGen, itemGen);
                }
                return itemGen + 1;
            }

            friend class TBlobStorageController::TTxConfigCmd;
            using TStatus = NKikimrBlobStorage::TConfigResponse::TStatus;
            void ExecuteStep(const NKikimrBlobStorage::TDefineBox& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TReadBox& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDeleteBox& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TMergeBoxes& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TUpdateDriveStatus& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TReadDriveStatus& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDefineHostConfig& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TReadHostConfig& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDeleteHostConfig& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDefineStoragePool& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TReadStoragePool& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDeleteStoragePool& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TProposeStoragePools& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TReassignGroupDisk& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TMoveGroups& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TQueryBaseConfig& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDropDonorDisk& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TAddDriveSerial& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TRemoveDriveSerial& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TForgetDriveSerial& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TMigrateToSerial& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TAllocateVirtualGroup& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TDecommitGroups& cmd, TStatus& status);
            void ExecuteStep(const NKikimrBlobStorage::TWipeVDisk& cmd, TStatus& status);
        };

    } // NBsController
} // NKikimr
