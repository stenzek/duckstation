#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cpuinfo.h>
#include <arm/api.h>
#include <cpuinfo/internal-api.h>
#include <cpuinfo/log.h>

#include <windows.h>

#ifdef __GNUC__
  #define CPUINFO_ALLOCA __builtin_alloca
#else
  #define CPUINFO_ALLOCA _alloca
#endif


static inline uint32_t bit_mask(uint32_t bits) {
	return (UINT32_C(1) << bits) - UINT32_C(1);
}

static inline uint32_t low_index_from_kaffinity(KAFFINITY kaffinity) {
	#if defined(_M_ARM64)
		unsigned long index;
		_BitScanForward64(&index, (unsigned __int64) kaffinity);
		return (uint32_t) index;
	#elif defined(_M_ARM)
		unsigned long index;
		_BitScanForward(&index, (unsigned long) kaffinity);
		return (uint32_t) index;
	#else
		#error Platform-specific implementation required
	#endif
}

static bool cpuinfo_arm_windows_is_wine(void) {
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == NULL) {
		return false;
	}

	return GetProcAddress(ntdll, "wine_get_version") != NULL;
}

BOOL CALLBACK cpuinfo_arm_windows_init(PINIT_ONCE init_once, PVOID parameter, PVOID* context) {
	struct cpuinfo_processor* processors = NULL;
	struct cpuinfo_core* cores = NULL;
	struct cpuinfo_cluster* clusters = NULL;
	struct cpuinfo_package* packages = NULL;
	uint32_t* core_efficiency_classes = NULL;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX processor_infos = NULL;

	HANDLE heap = GetProcessHeap();
	const bool is_wine = cpuinfo_arm_windows_is_wine();

	/* WINE doesn't implement GetMaximumProcessorGroupCount and aborts when calling it */
	const uint32_t max_group_count = is_wine ? 1 : (uint32_t) GetMaximumProcessorGroupCount();
	cpuinfo_log_debug("detected %"PRIu32" processor groups", max_group_count);

	uint32_t processors_count = 0;
	uint32_t* processors_per_group = (uint32_t*) CPUINFO_ALLOCA(max_group_count * sizeof(uint32_t));
	for (uint32_t i = 0; i < max_group_count; i++) {
		processors_per_group[i] = GetMaximumProcessorCount((WORD) i);
		cpuinfo_log_debug("detected %"PRIu32" processors in group %"PRIu32,
			processors_per_group[i], i);
		processors_count += processors_per_group[i];
	}

	uint32_t* processors_before_group = (uint32_t*) CPUINFO_ALLOCA(max_group_count * sizeof(uint32_t));
	for (uint32_t i = 0, count = 0; i < max_group_count; i++) {
		processors_before_group[i] = count;
		cpuinfo_log_debug("detected %"PRIu32" processors before group %"PRIu32,
			processors_before_group[i], i);
		count += processors_per_group[i];
	}

	processors = HeapAlloc(heap, HEAP_ZERO_MEMORY, processors_count * sizeof(struct cpuinfo_processor));
	if (processors == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" logical processors",
			processors_count * sizeof(struct cpuinfo_processor), processors_count);
		goto cleanup;
	}

	DWORD cores_info_size = 0;
	if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &cores_info_size) == FALSE) {
		const DWORD last_error = GetLastError();
		if (last_error != ERROR_INSUFFICIENT_BUFFER) {
			cpuinfo_log_error("failed to query size of processor cores information: error %"PRIu32,
				(uint32_t) last_error);
			goto cleanup;
		}
	}

	DWORD packages_info_size = 0;
	if (GetLogicalProcessorInformationEx(RelationProcessorPackage, NULL, &packages_info_size) == FALSE) {
		const DWORD last_error = GetLastError();
		if (last_error != ERROR_INSUFFICIENT_BUFFER) {
			cpuinfo_log_error("failed to query size of processor packages information: error %"PRIu32,
				(uint32_t) last_error);
			goto cleanup;
		}
	}

	DWORD max_info_size = max(cores_info_size, packages_info_size);

	processor_infos = HeapAlloc(heap, 0, max_info_size);
	if (processor_infos == NULL) {
		cpuinfo_log_error("failed to allocate %"PRIu32" bytes for logical processor information",
			(uint32_t) max_info_size);
		goto cleanup;
	}

	if (GetLogicalProcessorInformationEx(RelationProcessorPackage, processor_infos, &max_info_size) == FALSE) {
		cpuinfo_log_error("failed to query processor packages information: error %"PRIu32,
			(uint32_t) GetLastError());
		goto cleanup;
	}

	uint32_t packages_count = 0;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX packages_info_end =
		(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ((uintptr_t) processor_infos + packages_info_size);
	for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX package_info = processor_infos;
		package_info < packages_info_end;
		package_info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ((uintptr_t) package_info + package_info->Size))
	{
		if (package_info->Relationship != RelationProcessorPackage) {
			cpuinfo_log_warning("unexpected processor info type (%"PRIu32") for processor package information",
				(uint32_t) package_info->Relationship);
			continue;
		}

		/* We assume that packages are reported in APIC order */
		const uint32_t package_id = packages_count++;
		/* Iterate processor groups and set the package part of APIC ID */
		for (uint32_t i = 0; i < package_info->Processor.GroupCount; i++) {
			const uint32_t group_id = package_info->Processor.GroupMask[i].Group;
			/* Global index of the first logical processor belonging to this group */
			const uint32_t group_processors_start = processors_before_group[group_id];
			/* Bitmask representing processors in this group belonging to this package */
			KAFFINITY group_processors_mask = package_info->Processor.GroupMask[i].Mask;
			while (group_processors_mask != 0) {
				const uint32_t group_processor_id = low_index_from_kaffinity(group_processors_mask);
				const uint32_t processor_id = group_processors_start + group_processor_id;
				processors[processor_id].package = (const struct cpuinfo_package*) NULL + package_id;
				processors[processor_id].windows_group_id = (uint16_t) group_id;
				processors[processor_id].windows_processor_id = (uint16_t) group_processor_id;

				/* Reset the lowest bit in affinity mask */
				group_processors_mask &= (group_processors_mask - 1);
			}
		}
	}

	max_info_size = max(cores_info_size, packages_info_size);
	if (GetLogicalProcessorInformationEx(RelationProcessorCore, processor_infos, &max_info_size) == FALSE) {
		cpuinfo_log_error("failed to query processor cores information: error %"PRIu32,
			(uint32_t) GetLastError());
		goto cleanup;
	}

	uint32_t cores_count = 0;
	/* Index (among all cores) of the the first core on the current package */
	uint32_t package_core_start = 0;
	uint32_t current_package_apic_id = 0;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX cores_info_end =
		(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ((uintptr_t) processor_infos + cores_info_size);
	for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX core_info = processor_infos;
		core_info < cores_info_end;
		core_info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ((uintptr_t) core_info + core_info->Size))
	{
		if (core_info->Relationship != RelationProcessorCore) {
			cpuinfo_log_warning("unexpected processor info type (%"PRIu32") for processor core information",
				(uint32_t) core_info->Relationship);
			continue;
		}

		/* We assume that cores and logical processors are reported in APIC order */
		const uint32_t core_id = cores_count++;
		if (core_efficiency_classes == NULL)
			core_efficiency_classes = (uint32_t*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(uint32_t) * cores_count);
		else
			core_efficiency_classes = (uint32_t*)HeapReAlloc(heap, HEAP_ZERO_MEMORY, core_efficiency_classes, sizeof(uint32_t) * cores_count);
		core_efficiency_classes[core_id] = core_info->Processor.EfficiencyClass;

		uint32_t smt_id = 0;
		/* Iterate processor groups and set the core & SMT parts of APIC ID */
		for (uint32_t i = 0; i < core_info->Processor.GroupCount; i++) {
			const uint32_t group_id = core_info->Processor.GroupMask[i].Group;
			/* Global index of the first logical processor belonging to this group */
			const uint32_t group_processors_start = processors_before_group[group_id];
			/* Bitmask representing processors in this group belonging to this package */
			KAFFINITY group_processors_mask = core_info->Processor.GroupMask[i].Mask;
			while (group_processors_mask != 0) {
				const uint32_t group_processor_id = low_index_from_kaffinity(group_processors_mask);
				const uint32_t processor_id = group_processors_start + group_processor_id;

				/* Core ID w.r.t package */
				const uint32_t package_core_id = core_id - package_core_start;

				/* Set SMT ID (assume logical processors within the core are reported in APIC order) */
				processors[processor_id].smt_id = smt_id++;
				processors[processor_id].core = (const struct cpuinfo_core*) NULL + core_id;

				/* Reset the lowest bit in affinity mask */
				group_processors_mask &= (group_processors_mask - 1);
			}
		}
	}

	cores = HeapAlloc(heap, HEAP_ZERO_MEMORY, cores_count * sizeof(struct cpuinfo_core));
	if (cores == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" cores",
			cores_count * sizeof(struct cpuinfo_core), cores_count);
		goto cleanup;
	}

	clusters = HeapAlloc(heap, HEAP_ZERO_MEMORY, packages_count * sizeof(struct cpuinfo_cluster));
	if (clusters == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" core clusters",
			packages_count * sizeof(struct cpuinfo_cluster), packages_count);
		goto cleanup;
	}

	packages = HeapAlloc(heap, HEAP_ZERO_MEMORY, packages_count * sizeof(struct cpuinfo_package));
	if (packages == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" physical packages",
			packages_count * sizeof(struct cpuinfo_package), packages_count);
		goto cleanup;
	}

	for (uint32_t i = processors_count; i != 0; i--) {
		const uint32_t processor_id = i - 1;
		struct cpuinfo_processor* processor = processors + processor_id;

		/* Adjust core and package pointers for all logical processors */
		struct cpuinfo_core* core =
			(struct cpuinfo_core*) ((uintptr_t) cores + (uintptr_t) processor->core);
		processor->core = core;
		struct cpuinfo_cluster* cluster =
			(struct cpuinfo_cluster*) ((uintptr_t) clusters + (uintptr_t) processor->cluster);
		processor->cluster = cluster;
		struct cpuinfo_package* package =
			(struct cpuinfo_package*) ((uintptr_t) packages + (uintptr_t) processor->package);
		processor->package = package;

		/* This can be overwritten by lower-index processors on the same package */
		package->processor_start = processor_id;
		package->processor_count += 1;

		/* This can be overwritten by lower-index processors on the same cluster */
		cluster->processor_start = processor_id;
		cluster->processor_count += 1;

		/* This can be overwritten by lower-index processors on the same core*/
		core->processor_start = processor_id;
		core->processor_count += 1;
	}

	/* Set vendor/uarch/CPUID information for cores */
	for (uint32_t i = cores_count; i != 0; i--) {
		const uint32_t global_core_id = i - 1;
		struct cpuinfo_core* core = cores + global_core_id;
		const struct cpuinfo_processor* processor = processors + core->processor_start;
		struct cpuinfo_package* package = (struct cpuinfo_package*) processor->package;
		struct cpuinfo_cluster* cluster = (struct cpuinfo_cluster*) processor->cluster;

		core->cluster = cluster;
		core->package = package;
		core->core_id = global_core_id;
		core->vendor = cpuinfo_vendor_unknown;
		core->uarch  = cpuinfo_uarch_unknown;

		/* Lazy */
		core->frequency = core_efficiency_classes[global_core_id];

		/* This can be overwritten by lower-index cores on the same cluster/package */
		cluster->core_start = global_core_id;
		cluster->core_count += 1;
		package->core_start = global_core_id;
		package->core_count += 1;
	}

	for (uint32_t i = 0; i < packages_count; i++) {
		struct cpuinfo_package* package = packages + i;
		struct cpuinfo_cluster* cluster = clusters + i;

		cluster->package = package;
		cluster->vendor = cores[cluster->core_start].vendor;
		cluster->uarch = cores[cluster->core_start].uarch;
		package->cluster_start = i;
		package->cluster_count = 1;
	}


	/* Commit changes */
	cpuinfo_processors = processors;
	cpuinfo_cores = cores;
	cpuinfo_clusters = clusters;
	cpuinfo_packages = packages;

	cpuinfo_processors_count = processors_count;
	cpuinfo_cores_count = cores_count;
	cpuinfo_clusters_count = packages_count;
	cpuinfo_packages_count = packages_count;

	MemoryBarrier();

	cpuinfo_is_initialized = true;

	processors = NULL;
	cores = NULL;
	clusters = NULL;
	packages = NULL;

cleanup:
	if (core_efficiency_classes != NULL) {
		HeapFree(heap, 0, core_efficiency_classes);
	}
	if (processors != NULL) {
		HeapFree(heap, 0, processors);
	}
	if (cores != NULL) {
		HeapFree(heap, 0, cores);
	}
	if (clusters != NULL) {
		HeapFree(heap, 0, clusters);
	}
	if (packages != NULL) {
		HeapFree(heap, 0, packages);
	}
	return TRUE;
}
