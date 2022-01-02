using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx;
using BepInEx.Logging;
#if LFX_USE_IL2CPP
using BepInEx.IL2CPP;
using BepInEx.IL2CPP.Utils;
using UnhollowerRuntimeLib;
#endif

#if LFX_USE_UNITY_2019_3
using UnityEngine.LowLevel;
#else
using UnityEngine.Experimental.LowLevel;
#endif

namespace LatencyFleX
{
    public class Plugin
    {
        [DllImport("latencyflex_layer")]
        private static extern int lfx_WaitAndBeginFrame();

        [DllImport("latencyflex_wine")]
        private static extern int winelfx_WaitAndBeginFrame();

        private bool _isWine = false;

        private ManualLogSource _log;
        public Plugin(ManualLogSource log)
        {
            _log = log;
        }
        
        public void Run()
        {
            try
            {
                var method = GetType().GetMethod(nameof(lfx_WaitAndBeginFrame),
                    BindingFlags.NonPublic | BindingFlags.Static);
                Marshal.Prelink(method);
            }
            catch (DllNotFoundException)
            {
                _log.LogInfo("Direct DLL load failed: trying wine bridge");
                _isWine = true;
                try
                {
                    var method = GetType().GetMethod(nameof(winelfx_WaitAndBeginFrame),
                        BindingFlags.NonPublic | BindingFlags.Static);
                    Marshal.Prelink(method);
                }
                catch (DllNotFoundException)
                {
                    _log.LogError("Cannot find LatencyFleX runtime! Disabling plugin.");
                    return;
                }
            }

            var updateDelegate = (Action) (() =>
            {
                if (_isWine)
                {
                    winelfx_WaitAndBeginFrame();
                }
                else
                {
                    lfx_WaitAndBeginFrame();
                }
            });

#if LFX_USE_IL2CPP
            ClassInjector.RegisterTypeInIl2Cpp<LfxBeforeLoopInit>();
            var mySystem = new PlayerLoopSystemInternal
            {
                type = UnhollowerRuntimeLib.Il2CppType.Of<LfxBeforeLoopInit>(),
                updateDelegate = updateDelegate,
                numSubSystems = 0,
                updateFunction = System.IntPtr.Zero,
                loopConditionFunction = System.IntPtr.Zero,
            };
            
            var playerLoop = PlayerLoop.GetCurrentPlayerLoopInternal();
            
            var systems = new List<PlayerLoopSystemInternal>(playerLoop);
            // System 0 is the root node. It will never be executed
            systems[0].numSubSystems++;
            systems.Insert(1, mySystem);
            PlayerLoop.SetPlayerLoopInternal(systems.ToArray());
#else
            var mySystem = new PlayerLoopSystem
            {
                type = typeof(LfxBeforeLoopInit),
                updateDelegate = new PlayerLoopSystem.UpdateFunction(updateDelegate),
            };

#if LFX_USE_UNITY_2019_3
            var playerLoop = PlayerLoop.GetCurrentPlayerLoop();
#else
            var playerLoop = PlayerLoop.GetDefaultPlayerLoop();
#endif

            var initSubSystem = playerLoop.subSystemList[0];
            var subSystem = new List<PlayerLoopSystem>(initSubSystem.subSystemList);
            subSystem.Insert(0, mySystem);
            initSubSystem.subSystemList = subSystem.ToArray();
            playerLoop.subSystemList[0] = initSubSystem;

            PlayerLoop.SetPlayerLoop(playerLoop);
#endif

            _log.LogInfo("Plugin " + PluginInfo.PLUGIN_GUID + " is loaded!");
        }
#if LFX_USE_IL2CPP        
        private class LfxBeforeLoopInit: Il2CppSystem.Object {}
#else
        private class LfxBeforeLoopInit {}
#endif
    }
    
#if LFX_USE_IL2CPP
    [BepInPlugin(PluginInfo.PLUGIN_GUID, PluginInfo.PLUGIN_NAME, PluginInfo.PLUGIN_VERSION)]
    public class Il2CppPlugin : BasePlugin
    {
        public override void Load() {
            var plugin = new Plugin(Log);
            plugin.Run();
        }
    }
#else
    [BepInPlugin(PluginInfo.PLUGIN_GUID, PluginInfo.PLUGIN_NAME, PluginInfo.PLUGIN_VERSION)]
    public class MonoPlugin : BaseUnityPlugin
    {
        private void Awake()
        {
            var plugin = new Plugin(Logger);
            plugin.Run();
        }
    }
#endif
}