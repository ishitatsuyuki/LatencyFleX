using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx;
using UnityEngine.Experimental.LowLevel;

namespace LatencyFleX
{
    [BepInPlugin(PluginInfo.PLUGIN_GUID, PluginInfo.PLUGIN_NAME, PluginInfo.PLUGIN_VERSION)]
    public class Plugin : BaseUnityPlugin
    {
        [DllImport("latencyflex_layer")]
        private static extern int lfx_WaitAndBeginFrame();

        [DllImport("latencyflex_wine")]
        private static extern int winelfx_WaitAndBeginFrame();

        private bool _isWine = false;

        private void Awake()
        {
            try
            {
                var method = GetType().GetMethod("lfx_WaitAndBeginFrame", BindingFlags.NonPublic | BindingFlags.Static);
                Marshal.Prelink(method);
            }
            catch (DllNotFoundException)
            {
                Logger.LogInfo("Direct DLL load failed: trying wine bridge");
                _isWine = true;
                try
                {
                    var method = GetType().GetMethod("winelfx_WaitAndBeginFrame", BindingFlags.NonPublic | BindingFlags.Static);
                    Marshal.Prelink(method);
                }
                catch (DllNotFoundException)
                {
                    Logger.LogError("Cannot find LatencyFleX runtime! Disabling plugin.");
                    return;
                }
            }
            Logger.LogInfo("Plugin " + PluginInfo.PLUGIN_GUID + " is loaded!");

            var mySystem = new PlayerLoopSystem
            {
                type = typeof(LfxBeforeLoopInit),
                updateDelegate = () =>
                {
                    if (_isWine)
                    {
                        winelfx_WaitAndBeginFrame();
                    }
                    else
                    {
                        lfx_WaitAndBeginFrame();
                    }
                }
            };

            var playerLoop = PlayerLoop.GetDefaultPlayerLoop();

            var updateSystem = playerLoop.subSystemList[0]; // Initialization subsystem
            var subSystem = new List<PlayerLoopSystem>(updateSystem.subSystemList);
            subSystem.Insert(0, mySystem);
            updateSystem.subSystemList = subSystem.ToArray();
            playerLoop.subSystemList[0] = updateSystem;

            PlayerLoop.SetPlayerLoop(playerLoop);
        }

        public struct LfxBeforeLoopInit
        {
        }
    }
}