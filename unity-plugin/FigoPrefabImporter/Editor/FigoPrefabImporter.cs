// Figo Prefab Importer — thin editor glue around the bundled figo2unity
// converter. The converter itself does all the work (parse .fig/canvas.json,
// layout, bake sprites, emit .prefab YAML + textures + .meta); this window
// just stages the input, invokes the exe and refreshes the AssetDatabase.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Figo.PrefabImporter
{
    public class FigoPrefabImporterWindow : EditorWindow
    {
        public const string Version = "1.0.0";

        enum ColorSpaceMode { AutoDetect, Gamma, Linear }

        string inputPath = "";
        string outputFolder = "Assets/FigoImported";
        string fontsFolder = "";
        string frameFilter = "";
        float scale = 1f;
        bool extractComponents;
        ColorSpaceMode colorSpaceMode = ColorSpaceMode.AutoDetect;
        Vector2 logScroll;
        string lastLog = "";

        [MenuItem("Tools/Figo Prefab Importer...")]
        static void Open()
        {
            var w = GetWindow<FigoPrefabImporterWindow>("Figo Prefab Importer");
            w.minSize = new Vector2(420, 340);
        }

        // Right-click a .fig / canvas.json asset in the Project view.
        [MenuItem("Assets/Figo/Convert to UGUI Prefabs", true)]
        static bool ValidateConvertSelected()
        {
            var p = SelectedAssetPath();
            return p != null && IsSupportedInput(p);
        }

        [MenuItem("Assets/Figo/Convert to UGUI Prefabs")]
        static void ConvertSelected()
        {
            var w = GetWindow<FigoPrefabImporterWindow>("Figo Prefab Importer");
            w.inputPath = Path.GetFullPath(SelectedAssetPath());
            w.outputFolder = "Assets/FigoImported/" +
                Sanitize(Path.GetFileNameWithoutExtension(w.inputPath));
            w.Show();
        }

        static string SelectedAssetPath()
        {
            var o = Selection.activeObject;
            return o == null ? null : AssetDatabase.GetAssetPath(o);
        }

        static bool IsSupportedInput(string path)
        {
            var ext = Path.GetExtension(path).ToLowerInvariant();
            return ext == ".fig" || ext == ".json";
        }

        void OnGUI()
        {
            EditorGUILayout.Space(6);
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("Design input", EditorStyles.boldLabel);
                GUILayout.FlexibleSpace();
                GUILayout.Label("v" + Version, EditorStyles.miniLabel);
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                inputPath = EditorGUILayout.TextField(
                    new GUIContent(".fig / canvas.json",
                        "A Figma .fig file (File > Save local copy...), a figo canvas.json, or Figma REST JSON."),
                    inputPath);
                if (GUILayout.Button("Browse", GUILayout.Width(70)))
                {
                    var p = EditorUtility.OpenFilePanel("Select design file", "", "fig,json");
                    if (!string.IsNullOrEmpty(p)) inputPath = p;
                }
            }

            EditorGUILayout.Space(6);
            EditorGUILayout.LabelField("Output", EditorStyles.boldLabel);
            outputFolder = EditorGUILayout.TextField(
                new GUIContent("Folder under Assets"), outputFolder);

            EditorGUILayout.Space(6);
            EditorGUILayout.LabelField("Options", EditorStyles.boldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                fontsFolder = EditorGUILayout.TextField(
                    new GUIContent("Fonts folder (optional)",
                        "Folder with the design's .ttf/.otf files. They are bundled as font assets and matched by family/weight/italic; without it all text uses the built-in font."),
                    fontsFolder);
                if (GUILayout.Button("Browse", GUILayout.Width(70)))
                {
                    var p = EditorUtility.OpenFolderPanel("Select fonts folder", "", "");
                    if (!string.IsNullOrEmpty(p)) fontsFolder = p;
                }
            }
            frameFilter = EditorGUILayout.TextField(
                new GUIContent("Frame filter (optional)",
                    "Convert only the top-level frame with this name. Empty = every frame becomes a prefab."),
                frameFilter);
            scale = EditorGUILayout.FloatField(
                new GUIContent("Texture scale", "Supersampling factor for baked sprites (1 = design pixels)."),
                scale);
            extractComponents = EditorGUILayout.Toggle(
                new GUIContent("Extract shared components",
                    "Detect repeated UI (cards, buttons, rows) and emit them as nested prefabs in components/ with per-instance overrides."),
                extractComponents);
            colorSpaceMode = (ColorSpaceMode)EditorGUILayout.EnumPopup(
                new GUIContent("Color space",
                    "Gamma output matches the design pixel-for-pixel. Linear pre-compensates translucent alpha so glows/panels keep the sRGB look in Linear projects. Auto reads this project's PlayerSettings."),
                colorSpaceMode);

            var effectiveLinear = colorSpaceMode == ColorSpaceMode.Linear ||
                (colorSpaceMode == ColorSpaceMode.AutoDetect &&
                 PlayerSettings.colorSpace == ColorSpace.Linear);
            EditorGUILayout.HelpBox(
                effectiveLinear
                    ? "Exporting with Linear alpha pre-compensation (project color space: " + PlayerSettings.colorSpace + ")."
                    : "Exporting for Gamma color space (project color space: " + PlayerSettings.colorSpace + ").",
                MessageType.None);

            EditorGUILayout.Space(8);
            using (new EditorGUI.DisabledScope(!File.Exists(inputPath)))
            {
                if (GUILayout.Button("Convert", GUILayout.Height(30)))
                    Convert(effectiveLinear);
            }

            if (!string.IsNullOrEmpty(lastLog))
            {
                EditorGUILayout.Space(4);
                logScroll = EditorGUILayout.BeginScrollView(logScroll, GUILayout.ExpandHeight(true));
                EditorGUILayout.TextArea(lastLog, GUILayout.ExpandHeight(true));
                EditorGUILayout.EndScrollView();
            }
        }

        void Convert(bool linear)
        {
            var exe = FindConverter();
            if (exe == null)
            {
                EditorUtility.DisplayDialog("Figo Prefab Importer",
                    "The " + ConverterFileName() + " converter was not found next to the importer scripts (Editor/Bin/). Reimport the package.", "OK");
                return;
            }
            if (!outputFolder.Replace('\\', '/').StartsWith("Assets/"))
            {
                EditorUtility.DisplayDialog("Figo Prefab Importer",
                    "The output folder must be inside Assets/.", "OK");
                return;
            }

            // Stage the input outside Assets: .fig conversion writes a
            // <file>.fig.export/ cache next to the input, and canvas.json can
            // reference a sibling images/ directory — neither should be
            // imported as project assets.
            var stage = Path.Combine(Path.GetTempPath(), "figo-import-" + Process.GetCurrentProcess().Id);
            Directory.CreateDirectory(stage);
            var stagedInput = Path.Combine(stage, Path.GetFileName(inputPath));
            File.Copy(inputPath, stagedInput, true);
            var imagesDir = Path.Combine(Path.GetDirectoryName(inputPath) ?? "", "images");
            if (Directory.Exists(imagesDir))
                CopyDir(imagesDir, Path.Combine(stage, "images"));

            var outDir = Path.GetFullPath(outputFolder);
            Directory.CreateDirectory(outDir);

            var args = new List<string> { Quote(stagedInput), Quote(outDir) };
            if (!string.IsNullOrWhiteSpace(frameFilter)) { args.Add("--frame"); args.Add(Quote(frameFilter.Trim())); }
            if (!string.IsNullOrWhiteSpace(fontsFolder) && Directory.Exists(fontsFolder)) { args.Add("--fonts"); args.Add(Quote(fontsFolder)); }
            if (Math.Abs(scale - 1f) > 0.001f) { args.Add("--scale"); args.Add(scale.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)); }
            if (extractComponents) args.Add("--prefabs");
            if (linear) args.Add("--linear");

            var psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = string.Join(" ", args),
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };

            try
            {
                EditorUtility.DisplayProgressBar("Figo Prefab Importer", "Converting " + Path.GetFileName(inputPath) + "...", 0.4f);
                using (var p = Process.Start(psi))
                {
                    var stdout = p.StandardOutput.ReadToEnd();
                    var stderr = p.StandardError.ReadToEnd();
                    p.WaitForExit();
                    lastLog = (stdout + "\n" + stderr).Trim();
                    if (p.ExitCode != 0)
                    {
                        Debug.LogError("[Figo] conversion failed (exit " + p.ExitCode + ")\n" + lastLog);
                        EditorUtility.DisplayDialog("Figo Prefab Importer", "Conversion failed — see the Console for details.", "OK");
                        return;
                    }
                }
            }
            finally
            {
                EditorUtility.ClearProgressBar();
                try { Directory.Delete(stage, true); } catch { /* temp cleanup best-effort */ }
            }

            AssetDatabase.Refresh();
            Debug.Log("[Figo] " + lastLog + "\n-> " + outputFolder);
            var folderAsset = AssetDatabase.LoadAssetAtPath<UnityEngine.Object>(outputFolder.TrimEnd('/'));
            if (folderAsset != null) EditorGUIUtility.PingObject(folderAsset);
        }

        // Per-platform converter binary bundled in Editor/Bin (Windows .exe,
        // macOS mach-o with no extension).
        static string ConverterFileName()
        {
            return Application.platform == RuntimePlatform.OSXEditor
                ? "figo2unity" : "figo2unity.exe";
        }

        // The converter ships as a regular asset in Editor/Bin next to this
        // script; locate it relative to the script so the package folder can
        // be moved.
        static string FindConverter()
        {
            var guids = AssetDatabase.FindAssets("FigoPrefabImporter t:MonoScript");
            foreach (var g in guids)
            {
                var scriptPath = AssetDatabase.GUIDToAssetPath(g);
                var candidate = Path.Combine(Path.GetDirectoryName(scriptPath) ?? "", "Bin", ConverterFileName());
                if (!File.Exists(candidate)) continue;
                var full = Path.GetFullPath(candidate);
                // .unitypackage import does not preserve the executable bit —
                // restore it before launching (no-op when already set).
                if (Application.platform == RuntimePlatform.OSXEditor)
                {
                    try
                    {
                        using (var chmod = Process.Start(new ProcessStartInfo
                        {
                            FileName = "/bin/chmod",
                            Arguments = "+x " + Quote(full),
                            UseShellExecute = false,
                            CreateNoWindow = true,
                        }))
                        {
                            chmod.WaitForExit();
                        }
                    }
                    catch (Exception e)
                    {
                        Debug.LogWarning("[Figo] chmod +x failed: " + e.Message);
                    }
                }
                return full;
            }
            return null;
        }

        static void CopyDir(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (var f in Directory.GetFiles(src))
                File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
            foreach (var d in Directory.GetDirectories(src))
                CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
        }

        static string Quote(string s) => "\"" + s + "\"";

        static string Sanitize(string s)
        {
            var sb = new StringBuilder();
            foreach (var c in s)
                sb.Append(char.IsLetterOrDigit(c) || c == '_' || c == '-' ? c : '_');
            return sb.ToString();
        }
    }
}
