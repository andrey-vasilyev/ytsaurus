#pragma once

#include "public.h"

#include <yt/yt/core/ytree/interned_attributes.h>

// NB: interned attributes are serialized and saved as strings.
// Do not change those snake-case identifiers lightly!

#define FOR_EACH_INTERNED_ATTRIBUTE(XX) \
    XX(Abc, abc) \
    XX(AccessCounter, access_counter) \
    XX(AccessTime, access_time) \
    XX(Account, account) \
    XX(AccountStatistics, account_statistics) \
    XX(AccountResourceUsageLeaseIds, account_resource_usage_lease_ids) \
    XX(Acl, acl) \
    XX(ActionId, action_id) \
    XX(ActionIds, action_ids) \
    XX(ActualTabletState, actual_tablet_state) \
    XX(AcquisitionTime, acquisition_time) \
    XX(Addresses, addresses) \
    XX(AlertCount, alert_count) \
    XX(Alerts, alerts) \
    XX(Aliases, aliases) \
    XX(AlienConfigVersions, alien_config_versions) \
    XX(AllowChildrenLimitOvercommit, allow_children_limit_overcommit) \
    XX(AllowedProfilingTags, allowed_profiling_tags) \
    XX(AllowAggressivePreemption, allow_aggressive_preemption) \
    XX(AllowAggressiveStarvationPreemption, allow_aggressive_starvation_preemption) \
    XX(Annotation, annotation) \
    XX(AnnotationPath, annotation_path) \
    XX(Annotations, annotations) \
    XX(ApprovedReplicaCount, approved_replica_count) \
    XX(Areas, areas) \
    XX(AreaNodes, area_nodes) \
    XX(Atomicity, atomicity) \
    XX(AttributeKey, attribute_key) \
    XX(AttributeRevision, attribute_revision) \
    XX(Available, available) \
    XX(AvailableSpace, available_space) \
    XX(AvailableSpacePerMedium, available_space_per_medium) \
    XX(BackupState, backup_state) \
    XX(Banned, banned) \
    XX(BannedNodeCount, banned_node_count) \
    XX(BoundaryKeys, boundary_keys) \
    XX(BranchedNodeIds, branched_node_ids) \
    XX(Broken, broken) \
    XX(Builtin, builtin) \
    XX(Cache, cache) \
    XX(CachedReplicas, cached_replicas) \
    XX(Cellars, cellars) \
    XX(CellBalancerConfig, cell_balancer_config) \
    XX(CellBundle, cell_bundle) \
    XX(CellBundleId, cell_bundle_id) \
    XX(CellId, cell_id) \
    XX(CellIds, cell_ids) \
    XX(CellTag, cell_tag) \
    XX(ChangelogAccountViolatedResourceLimits, changelog_account_violated_resource_limits) \
    XX(ChaosOptions, chaos_options) \
    XX(ChildCount, child_count) \
    XX(ChildIds, child_ids) \
    XX(ChildKey, child_key) \
    XX(ChunkCount, chunk_count) \
    XX(ChunkFormat, chunk_format) \
    XX(ChunkFormatStatistics, chunk_format_statistics) \
    XX(ChunkId, chunk_id) \
    XX(ChunkIds, chunk_ids) \
    XX(ChunkListId, chunk_list_id) \
    XX(ChunkMergerMode, chunk_merger_mode) \
    XX(ChunkMergerNodeTraversalConcurrency, chunk_merger_node_traversal_concurrency) \
    XX(ChunkMergerTraversalInfo, chunk_merger_traversal_info) \
    XX(ChunkReader, chunk_reader) \
    XX(ChunkReplicaCount, chunk_replica_count) \
    XX(ChunkReplicatorEnabled, chunk_replicator_enabled) \
    XX(ChunkRefreshEnabled, chunk_refresh_enabled) \
    XX(ChunkRequisitionUpdateEnabled, chunk_requisition_update_enabled) \
    XX(ChunkSealerEnabled, chunk_sealer_enabled) \
    XX(ChunkRowCount, chunk_row_count) \
    XX(ChunkType, chunk_type) \
    XX(ChunkWriter, chunk_writer) \
    XX(ClusterConnection, cluster_connection) \
    XX(ClusterName, cluster_name) \
    XX(CommitOrdering, commit_ordering) \
    XX(CommittedResourceUsage, committed_resource_usage) \
    XX(CompressedDataSize, compressed_data_size) \
    XX(CompressionCodec, compression_codec) \
    XX(CompressionRatio, compression_ratio) \
    XX(CompressionStatistics, compression_statistics) \
    XX(CollocationType, collocation_type) \
    XX(Config, config) \
    XX(ConfigPreset, config_preset) \
    XX(ConfigVersion, config_version) \
    XX(Confirmed, confirmed) \
    XX(ConsistentReplicaPlacement, consistent_replica_placement) \
    XX(ConsistentReplicaPlacementHash, consistent_replica_placement_hash) \
    XX(ConsistentReplicaPlacementTokenCount, consistent_replica_placement_token_count) \
    XX(ContentRevision, content_revision) \
    XX(CountByHealth, count_by_health) \
    XX(CreateEphemeralSubpools, create_ephemeral_subpools) \
    XX(CreationTime, creation_time) \
    XX(CumulativeStatistics, cumulative_statistics) \
    XX(CurrentCommitRevision, current_commit_revision) \
    XX(CurrentMountTransactionId, current_mount_transaction_id) \
    XX(CustomProfilingTagFilter, custom_profiling_tag_filter) \
    XX(DataCenter, data_center) \
    XX(DataWeight, data_weight) \
    XX(Deadline, deadline) \
    XX(Decommissioned, decommissioned) \
    XX(DecommissionedNodeCount, decommissioned_node_count) \
    XX(DependentTransactionIds, dependent_transaction_ids) \
    XX(DescriptorConfigVersion, descriptor_config_version) \
    XX(Depth, depth) \
    XX(DesiredTabletCount, desired_tablet_count) \
    XX(DesiredTabletSize, desired_tablet_size) \
    XX(DestroyedChunkReplicaCount, destroyed_chunk_replica_count) \
    XX(DetailedMasterMemoryUsage, detailed_master_memory_usage) \
    XX(DisableSchedulerJobs, disable_scheduler_jobs) \
    XX(DisableTabletBalancer, disable_tablet_balancer) \
    XX(DisableTabletCells, disable_tablet_cells) \
    XX(DisableWriteSessions, disable_write_sessions) \
    XX(DiskSpace, disk_space) \
    XX(Dynamic, dynamic) \
    XX(DynamicConfigVersion, dynamic_config_version) \
    XX(DynamicOptions, dynamic_options) \
    XX(Eden, eden) \
    XX(EffectiveAcl, effective_acl) \
    XX(EnableAggressiveStarvation, enable_aggressive_starvation) \
    XX(EnableByUserProfiling, enable_by_user_profiling) \
    XX(EnableConsistentChunkReplicaPlacement, enable_consistent_chunk_replica_placement) \
    XX(EnableDetailedLogs, enable_detailed_logs) \
    XX(EnableDetailedProfiling, enable_detailed_profiling) \
    XX(EnableFairShareTruncationInFifoPool, enable_fair_share_truncation_in_fifo_pool) \
    XX(EnableTabletBalancer, enable_tablet_balancer) \
    XX(EnableDynamicStoreRead, enable_dynamic_store_read) \
    XX(EnableReplicatedTableTracker, enable_replicated_table_tracker) \
    XX(EnableSkynetSharing, enable_skynet_sharing) \
    XX(EndorsementRequired, endorsement_required) \
    XX(EntranceCellTag, entrance_cell_tag) \
    XX(EntranceNodeId, entrance_node_id) \
    XX(EphemeralRefCounter, ephemeral_ref_counter) \
    XX(EphemeralSubpoolConfig, ephemeral_subpool_config) \
    XX(ErasureCodec, erasure_codec) \
    XX(ErasureStatistics, erasure_statistics) \
    XX(Error, error) \
    XX(ErrorCount, error_count) \
    XX(Errors, errors) \
    XX(ErrorsUntrimmed, errors_untrimmed) \
    XX(EstimatedCreationTime, estimated_creation_time) \
    XX(Executable, executable) \
    XX(ExitCellTag, exit_cell_tag) \
    XX(ExitNodeId, exit_node_id) \
    XX(ExpectedState, expected_state) \
    XX(ExpectedTabletState, expected_tablet_state) \
    XX(ExpirationTime, expiration_time) \
    XX(ExpirationTimeout, expiration_timeout) \
    XX(ExportedObjectCount, exported_object_count) \
    XX(ExportedObjects, exported_objects) \
    XX(Exports, exports) \
    XX(External, external) \
    XX(ExternalizedToCellTags, externalized_to_cell_tags) \
    XX(ExternalCellTag, external_cell_tag) \
    XX(ExternalRequisitionIndexes, external_requisition_indexes) \
    XX(ExternalRequisitions, external_requisitions) \
    XX(FairSharePreemptionTimeout, fair_share_preemption_timeout) \
    XX(FairSharePreemptionTimeoutLimit, fair_share_preemption_timeout_limit) \
    XX(FairShareStarvationTimeout, fair_share_starvation_timeout) \
    XX(FairShareAggressiveStarvationTimeout, fair_share_aggressive_starvation_timeout) \
    XX(FairShareStarvationTolerance, fair_share_starvation_tolerance) \
    XX(FifoSortParameters, fifo_sort_parameters) \
    XX(FileName, file_name) \
    XX(Flavors, flavors) \
    XX(FlushLagTime, flush_lag_time) \
    XX(FlushedRowCount, flushed_row_count) \
    XX(FolderId, folder_id) \
    XX(ForcedCompactionRevision, forced_compaction_revision) \
    XX(ForcedStoreCompactionRevision, forced_store_compaction_revision) \
    XX(ForcedHunkCompactionRevision, forced_hunk_compaction_revision) \
    XX(ForbidImmediateOperations, forbid_immediate_operations) \
    XX(ForbidImmediateOperationsInRoot, forbid_immediate_operations_in_root) \
    XX(Foreign, foreign) \
    XX(FirstOverlayedRowIndex, first_overlayed_row_index) \
    XX(Freeze, freeze) \
    XX(Full, full) \
    XX(FullNodeCount, full_node_count) \
    XX(Health, health) \
    XX(HistoricUsageConfig, historic_usage_config) \
    XX(Host, host) \
    XX(Hosts, hosts) \
    XX(HunkChunkRefs, hunk_chunk_refs) \
    XX(HunkChunkReader, hunk_chunk_reader) \
    XX(HunkChunkWriter, hunk_chunk_writer) \
    XX(HunkCount, hunk_count) \
    XX(HunkStatistics, hunk_statistics) \
    XX(HydraReadOnly, hydra_read_only) \
    XX(Id, id) \
    XX(Implicit, implicit) \
    XX(ImportRefCounter, import_ref_counter) \
    XX(ImportedObjectCount, imported_object_count) \
    XX(ImportedObjectIds, imported_object_ids) \
    XX(InMemoryMode, in_memory_mode) \
    XX(Index, index) \
    XX(InferChildrenWeightsFromHistoricUsage, infer_children_weights_from_historic_usage) \
    XX(InheritAcl, inherit_acl) \
    XX(IntegralGuarantees, integral_guarantees) \
    XX(IOWeights, io_weights) \
    XX(IsBeingMerged, is_being_merged) \
    XX(Jobs, jobs) \
    XX(KeepFinished, keep_finished) \
    XX(Key, key) \
    XX(KeyColumns, key_columns) \
    XX(Kind, kind) \
    XX(LastAttributesUpdateTime, last_attributes_update_time) \
    XX(LastCommitTimestamp, last_commit_timestamp) \
    XX(LastHeartbeatTime, last_heartbeat_time) \
    XX(LastMountTransactionId, last_mount_transaction_id) \
    XX(LastPingTime, last_ping_time) \
    XX(LastSeenReplicas, last_seen_replicas) \
    XX(LastSeenTime, last_seen_time) \
    XX(LastWriteTimestamp, last_write_timestamp) \
    XX(LeadingPeerId, leading_peer_id) \
    XX(LeaseTransactionId, lease_transaction_id) \
    XX(LifeStage, life_stage) \
    XX(LocalHealth, local_health) \
    XX(LocalRequisition, local_requisition) \
    XX(LocalRequisitionIndex, local_requisition_index) \
    XX(LockCount, lock_count) \
    XX(LockIds, lock_ids) \
    XX(LockMode, lock_mode) \
    XX(LockedNodeIds, locked_node_ids) \
    XX(Locks, locks) \
    XX(LowerLimit, lower_limit) \
    XX(MasterAlerts, master_alerts) \
    XX(MasterCacheNodes, master_cache_nodes) \
    XX(MasterMetaSize, master_meta_size) \
    XX(MaxBlockSize, max_block_size) \
    XX(MaxChangelogId, max_changelog_id) \
    XX(MaxKey, max_key) \
    XX(MaxRunningOperationCount, max_running_operation_count) \
    XX(MaxOperationCount, max_operation_count) \
    XX(MaxShareRatio, max_share_ratio) \
    XX(MaxSnapshotId, max_snapshot_id) \
    XX(MaxTabletSize, max_tablet_size) \
    XX(MaxTimestamp, max_timestamp) \
    XX(MD5, md5) \
    XX(Media, media) \
    XX(MemberCount, member_count) \
    XX(MemberOf, member_of) \
    XX(MemberOfClosure, member_of_closure) \
    XX(Members, members) \
    XX(MemoryUsage, memory_usage) \
    XX(MergeJobRateLimit, merge_job_rate_limit) \
    XX(MetaSize, meta_size) \
    XX(MeteringTags, metering_tags) \
    XX(MinChildHeapSize, min_child_heap_size) \
    XX(MinKey, min_key) \
    XX(MinShareResources, min_share_resources) \
    XX(MinTabletSize, min_tablet_size) \
    XX(MinTimestamp, min_timestamp) \
    XX(Mixed, mixed) \
    XX(Mode, mode) \
    XX(ModificationTime, modification_time) \
    XX(MountRevision, mount_revision) \
    XX(Movable, movable) \
    XX(MulticellCount, multicell_count) \
    XX(MulticellResourceUsage, multicell_resource_usage) \
    XX(MulticellStates, multicell_states) \
    XX(MulticellStatistics, multicell_statistics) \
    XX(MulticellStatus, multicell_status) \
    XX(Name, name) \
    XX(Namespace, namespace) \
    XX(NativeCellTag, native_cell_tag) \
    XX(NativeContentRevision, native_content_revision) \
    XX(NestedTransactionIds, nested_transaction_ids) \
    XX(NodeId, node_id) \
    XX(Nodes, nodes) \
    XX(NodeTagFilter, node_tag_filter) \
    XX(Offline, offline) \
    XX(OfflineNodeCount, offline_node_count) \
    XX(Online, online) \
    XX(OnlineNodeCount, online_node_count) \
    XX(Opaque, opaque) \
    XX(OpaqueAttributeKeys, opaque_attribute_keys) \
    XX(OptimizeFor, optimize_for) \
    XX(OptimizeForStatistics, optimize_for_statistics) \
    XX(Options, options) \
    XX(Overlayed, overlayed) \
    XX(Owner, owner) \
    XX(OwningNodes, owning_nodes) \
    XX(Partitions, partitions) \
    XX(PartitionedBy, partitioned_by) \
    XX(Packing, packing) \
    XX(ParentId, parent_id) \
    XX(ParentIds, parent_ids) \
    XX(ParentName, parent_name) \
    XX(PartLossTime, part_loss_time) \
    XX(Path, path) \
    XX(PeerCount, peer_count) \
    XX(Peers, peers) \
    XX(PerformanceCounters, performance_counters) \
    XX(PivotKey, pivot_key) \
    XX(PivotKeys, pivot_keys) \
    XX(PoolConfigPresets, pool_config_presets) \
    XX(PreloadState, preload_state) \
    XX(PrerequisiteTransactionId, prerequisite_transaction_id) \
    XX(PrerequisiteTransactionIds, prerequisite_transaction_ids) \
    XX(PreserveTimestamps, preserve_timestamps) \
    XX(PrimaryCellId, primary_cell_id) \
    XX(PrimaryCellTag, primary_cell_tag) \
    XX(PrimaryMedium, primary_medium) \
    XX(Priority, priority) \
    XX(ProfilingMode, profiling_mode) \
    XX(ProfilingTag, profiling_tag) \
    XX(ProjectId, project_id) \
    XX(ProxyKind, proxy_kind) \
    XX(QuorumInfo, quorum_info) \
    XX(QuorumRowCount, quorum_row_count) \
    XX(Rack, rack) \
    XX(Racks, racks) \
    XX(ReadQuorum, read_quorum) \
    XX(ReadRequestRateLimit, read_request_rate_limit) \
    XX(RecursiveCommittedResourceUsage, recursive_committed_resource_usage) \
    XX(RecursiveResourceUsage, recursive_resource_usage) \
    XX(RecursiveViolatedResourceLimits, recursive_violated_resource_limits) \
    XX(RefCounter, ref_counter) \
    XX(ReferencingAccounts, referencing_accounts) \
    XX(RegisterTime, register_time) \
    XX(Registered, registered) \
    XX(RegisteredMasterCellTags, registered_master_cell_tags) \
    XX(RemovalStarted, removal_started) \
    XX(ReplicaLagLimit, replica_lag_limit) \
    XX(ReplicaPath, replica_path) \
    XX(Replicas, replicas) \
    XX(ReplicatedTableOptions, replicated_table_options) \
    XX(ReplicationCollocationId, replication_collocation_id) \
    XX(ReplicationCollocationTablePaths, replication_collocation_table_paths) \
    XX(ReplicationErrorCount, replication_error_count) \
    XX(ReplicationErrors, replication_errors) \
    XX(ReplicationFactor, replication_factor) \
    XX(ReplicationLagTime, replication_lag_time) \
    XX(ReplicationProgress, replication_progress) \
    XX(ReplicationStatus, replication_status) \
    XX(RequestQueueSizeLimit, request_queue_size_limit) \
    XX(RequestLimits, request_limits) \
    XX(Requisition, requisition) \
    XX(ResourceLimits, resource_limits) \
    XX(ResourceLimitsOverrides, resource_limits_overrides) \
    XX(ResourceUsage, resource_usage) \
    XX(RetainedTimestamp, retained_timestamp) \
    XX(Revision, revision) \
    XX(RootNodeId, root_node_id) \
    XX(RowCount, row_count) \
    XX(ScanFlags, scan_flags) \
    XX(SchedulingTag, scheduling_tag) \
    XX(SchedulingTagFilter, scheduling_tag_filter) \
    XX(Schema, schema) \
    XX(SchemaDuplicateCount, schema_duplicate_count) \
    XX(SchemaId, schema_id) \
    XX(SchemaMode, schema_mode) \
    XX(Sealed, sealed) \
    XX(SharedToSkynet, shared_to_skynet) \
    XX(ReplicatedToCellTags, replicated_to_cell_tags) \
    XX(ResolveCached, resolve_cached) \
    XX(SecurityTags, security_tags) \
    XX(ShardId, shard_id) \
    XX(SkipFreezing, skip_freezing) \
    XX(SnapshotAccountViolatedResourceLimits, snapshot_account_violated_resource_limits) \
    XX(Sorted, sorted) \
    XX(SortedBy, sorted_by) \
    XX(StagedNodeIds, staged_node_ids) \
    XX(StagedObjectIds, staged_object_ids) \
    XX(StagingAccount, staging_account) \
    XX(StagingTransactionId, staging_transaction_id) \
    XX(StartReplicationTimestamp, start_replication_timestamp) \
    XX(StartTime, start_time) \
    XX(State, state) \
    XX(Statistics, statistics) \
    XX(Status, status) \
    XX(StoredReplicas, stored_replicas) \
    XX(StoresUpdatePrepared, stores_update_prepared) \
    XX(StoresUpdatePreparedTransactionId, stores_update_prepared_transaction_id) \
    XX(StrongGuaranteeResources, strong_guarantee_resources) \
    XX(TableChunkFormat, table_chunk_format) \
    XX(TableChunkFormatStatistics, table_chunk_format_statistics) \
    XX(TableId, table_id) \
    XX(TableIds, table_ids) \
    XX(TablePath, table_path) \
    XX(TablePaths, table_paths) \
    XX(TabletActions, tablet_actions) \
    XX(TabletBalancerConfig, tablet_balancer_config) \
    XX(TabletBackupState, tablet_backup_state) \
    XX(TabletCellBundle, tablet_cell_bundle) \
    XX(TabletCellCount, tablet_cell_count) \
    XX(TabletCellIds, tablet_cell_ids) \
    XX(TabletCellLifeStage, tablet_cell_life_stage) \
    XX(TabletCount, tablet_count) \
    XX(TabletCountByState, tablet_count_by_state) \
    XX(TabletCountByExpectedState, tablet_count_by_expected_state) \
    XX(TabletErrorCount, tablet_error_count) \
    XX(TabletErrors, tablet_errors) \
    XX(TabletErrorsUntrimmed, tablet_errors_untrimmed) \
    XX(TabletId, tablet_id) \
    XX(TabletIds, tablet_ids) \
    XX(TabletSlots, tablet_slots) \
    XX(TabletState, tablet_state) \
    XX(TabletStatistics, tablet_statistics) \
    XX(Tablets, tablets) \
    XX(Tags, tags) \
    XX(TargetPath, target_path) \
    XX(Timeout, timeout) \
    XX(Timestamp, timestamp) \
    XX(TimestampProviderNodes, timestamp_provider_nodes) \
    XX(Title, title) \
    XX(TotalHunkLength, total_hunk_length) \
    XX(TotalAccountStatistics, total_account_statistics) \
    XX(TotalChildrenResourceLimits, total_children_resource_limits) \
    XX(TotalCommittedResourceUsage, total_committed_resource_usage) \
    XX(TotalResourceLimits, total_resource_limits) \
    XX(TotalResourceUsage, total_resource_usage) \
    XX(TotalStatistics, total_statistics) \
    XX(TransactionId, transaction_id) \
    XX(Transient, transient) \
    XX(Tree, tree) \
    XX(TrimmedChildCount, trimmed_child_count) \
    XX(TrimmedRowCount, trimmed_row_count) \
    XX(TruncateFifoPoolUnsatisfiedChildFairShare, truncate_fifo_pool_unsatisfied_child_fair_share) \
    XX(Type, type) \
    XX(UncompressedDataSize, uncompressed_data_size) \
    XX(UnconfirmedDynamicTableLocks, unconfirmed_dynamic_table_locks) \
    XX(UnflushedTimestamp, unflushed_timestamp) \
    XX(UnmergedRowCount, unmerged_row_count) \
    XX(Unregistered, unregistered) \
    XX(UpdateMode, update_mode) \
    XX(UpperLimit, upper_limit) \
    XX(UpstreamReplicaId, upstream_replica_id) \
    XX(ReplicationCardToken, replication_card_token) \
    XX(UsableAccounts, usable_accounts) \
    XX(UsableNetworkProjects, usable_network_projects) \
    XX(UsableTabletCellBundles, usable_tablet_cell_bundles) \
    XX(UsedSpace, used_space) \
    XX(UsedSpacePerMedium, used_space_per_medium) \
    XX(UserAttributes, user_attributes) \
    XX(UserAttributeKeys, user_attribute_keys) \
    XX(UserTags, user_tags) \
    XX(Value, value) \
    XX(ValueCount, value_count) \
    XX(Version, version) \
    XX(ViolatedResourceLimits, violated_resource_limits) \
    XX(Vital, vital) \
    XX(Weight, weight) \
    XX(WeakRefCounter, weak_ref_counter) \
    XX(WithAlertsNodeCount, with_alerts_node_count) \
    XX(WriteQuorum, write_quorum) \
    XX(WriteRequestRateLimit, write_request_rate_limit) \

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

// Don't litter the namespace, yet at the same time make the "enum" items
// implicitly castable to an integral type by switch().
class EInternedAttributeKey
{
    enum : size_t
    {
        InvalidKey__Code = size_t(NYTree::InvalidInternedAttribute),
        Count__Code = size_t(NYTree::CountInternedAttribute),

#define XX(camelCaseName, snakeCaseName) camelCaseName##__Code,
    FOR_EACH_INTERNED_ATTRIBUTE(XX)
#undef XX
    };

public:
    static constexpr NYTree::TInternedAttributeKey InvalidKey = NYTree::InvalidInternedAttribute;
    static constexpr NYTree::TInternedAttributeKey Count = NYTree::CountInternedAttribute;

#define XX(camelCaseName, snakeCaseName) \
    static constexpr NYTree::TInternedAttributeKey camelCaseName{camelCaseName##__Code};

    FOR_EACH_INTERNED_ATTRIBUTE(XX)
#undef XX
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
