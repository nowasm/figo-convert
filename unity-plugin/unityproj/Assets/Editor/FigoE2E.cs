// Batchmode e2e for the download-on-first-use converter flow (not part of
// the shipped package — lives outside Assets/FigoPrefabImporter).
// Run: Unity -batchmode -projectPath ... -executeMethod Figo.PrefabImporter.FigoE2E.Run -quit
using System;
using System.IO;
using System.Reflection;
using UnityEditor;
using UnityEngine;

namespace Figo.PrefabImporter
{
    public static class FigoE2E
    {
        public static void Run()
        {
            try
            {
                // start from a clean slate: no download cache, no previous output
                var cache = Path.GetFullPath(Path.Combine("Library", "FigoPrefabImporter"));
                if (Directory.Exists(cache)) Directory.Delete(cache, true);
                var outFolder = "Assets/FigoE2EOut";
                if (Directory.Exists(outFolder)) { Directory.Delete(outFolder, true); File.Delete(outFolder + ".meta"); }

                var t = Type.GetType("Figo.PrefabImporter.FigoPrefabImporterWindow, Figo.PrefabImporter.Editor");
                if (t == null) throw new Exception("window type not found");
                var w = ScriptableObject.CreateInstance(t);
                const BindingFlags F = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
                t.GetField("inputPath", F).SetValue(w, Path.GetFullPath("Assets/FigoPrefabImporter/Samples/starfall_menu.canvas.json"));
                t.GetField("outputFolder", F).SetValue(w, outFolder);
                t.GetMethod("Convert", F).Invoke(w, new object[] { false });

                var log = (string)t.GetField("lastLog", F).GetValue(w);
                var exe = Path.Combine(cache, Application.platform == RuntimePlatform.OSXEditor ? "figo2unity" : "figo2unity.exe");
                var prefabs = Directory.Exists(outFolder) ? Directory.GetFiles(outFolder, "*.prefab", SearchOption.AllDirectories) : new string[0];
                Debug.Log("[FigoE2E] converter downloaded: " + File.Exists(exe) + " (" + (File.Exists(exe) ? new FileInfo(exe).Length : 0) + " bytes)");
                Debug.Log("[FigoE2E] converter log: " + log);
                Debug.Log("[FigoE2E] prefabs: " + prefabs.Length);
                if (!File.Exists(exe)) throw new Exception("converter was not downloaded to " + exe);
                if (prefabs.Length < 2) throw new Exception("expected >= 2 prefabs, got " + prefabs.Length);
                Debug.Log("[FigoE2E] RESULT: OK");
                EditorApplication.Exit(0);
            }
            catch (Exception e)
            {
                Debug.LogError("[FigoE2E] FAILED: " + e);
                EditorApplication.Exit(1);
            }
        }
    }
}
