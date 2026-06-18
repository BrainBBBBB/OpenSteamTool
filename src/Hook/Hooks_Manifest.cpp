#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include <format>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        if (!result) return result;

        // ── Beta branch unlock ──────────────────────────────────────────
        // For any depot belonging to an "addappid"-managed app, force
        //   LcsRequired = 0     → Steam stops checking for a beta-access
        //                         subscription on this depot.
        // And clear the "betaFallback" flag so the user-selected branch
        // (staging/development/etc.) is actually used instead of public.
        // This makes the "Betas" dropdown in game properties functional
        // for unowned games unlocked via Lua.
        bool patchedBeta = false;
        if (pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                if (LuaConfig::HasDepot(e.AppId) || LuaConfig::HasDepot(e.DepotId)) {
                    if (e.LcsRequired) {
                        LOG_MANIFEST_INFO("BuildDepotDependency: clearing LcsRequired for depot {} (app {})",
                            e.DepotId, e.AppId);
                        e.LcsRequired = 0;
                        patchedBeta = true;
                    }
                }
            }
        }
        if (pSharedDepotInfo && pSharedDepotInfo->m_Size) {
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                DepotEntry& e = pSharedDepotInfo->m_Memory.m_pMemory[i];
                if (LuaConfig::HasDepot(e.AppId) || LuaConfig::HasDepot(e.DepotId)) {
                    if (e.LcsRequired) {
                        LOG_MANIFEST_INFO("BuildDepotDependency: clearing LcsRequired for shared depot {} (app {})",
                            e.DepotId, e.AppId);
                        e.LcsRequired = 0;
                        patchedBeta = true;
                    }
                }
            }
        }
        if (pbBetaFallback && *pbBetaFallback && LuaConfig::HasDepot(AppId)) {
            LOG_MANIFEST_INFO("BuildDepotDependency: clearing betaFallback flag for app {}", AppId);
            *pbBetaFallback = false;
            patchedBeta = true;
        }
        (void)patchedBeta;

        // ── Manifest pinning ────────────────────────────────────────────
        // Only patch manifests when Steam reports gid=0 for a depot we
        // have an override for.  When Steam already has a non-zero gid
        // (for example, the user picked a beta branch and Steam has
        // resolved that branch's manifest from appinfo), we MUST NOT
        // overwrite it -- doing so makes Steam ask for the wrong manifest
        // and the download silently fails.
        const auto& overrides = LuaConfig::GetManifestOverrides();
        if (overrides.empty()) return result;

        if (pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    if (e.ManifestGid != 0) {
                        LOG_MANIFEST_INFO("BuildDepotDependency: skip override for depot {} -- Steam already has gid {} (likely a beta branch)",
                            e.DepotId, e.ManifestGid);
                        continue;
                    }
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }
        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }
}
