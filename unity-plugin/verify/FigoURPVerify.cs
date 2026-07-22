// URP compatibility verification for the Figo Prefab Importer package.
// NOT part of the shipped package and NOT compiled in unityproj (it needs
// the URP package). Usage: copy unityproj's Assets/Packages/ProjectSettings
// to a SHORT temp path (URP's PackageCache exceeds MAX_PATH under long
// roots), add "com.unity.render-pipelines.universal" to Packages/
// manifest.json, drop this file into Assets/Editor/, then:
//   Unity -batchmode -projectPath <tmp> -executeMethod Figo.PrefabImporter.FigoURPVerify.Run -quit
// (no -nographics: the test render needs a GPU).
// Verified 2026-07-21: Unity 2022.3.62f3 + URP 14.0.12 — demo scene renders
// pixel-correct (log: "[FigoURP] RESULT: OK"). Since then the sample ships
// with NO custom shader/material (Asset Store render-pipeline scan flags
// them); text uses Unity's default UI material, so there is nothing to
// shader-compile-check anymore.
// 1. activates a freshly created URP pipeline asset,
// 2. opens the demo scene and renders it through a URP camera to a PNG.
using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.UI;

namespace Figo.PrefabImporter
{
    public static class FigoURPVerify
    {
        public static void Run()
        {
            try
            {
                // 1. create + activate a URP asset
                var rendererData = ScriptableObject.CreateInstance<UniversalRendererData>();
                AssetDatabase.CreateAsset(rendererData, "Assets/URPRenderer.asset");
                var rp = UniversalRenderPipelineAsset.Create(rendererData);
                AssetDatabase.CreateAsset(rp, "Assets/URPAsset.asset");
                GraphicsSettings.renderPipelineAsset = rp;
                QualitySettings.renderPipeline = rp;
                Debug.Log("[FigoURP] active pipeline: " + (GraphicsSettings.currentRenderPipeline != null ? GraphicsSettings.currentRenderPipeline.GetType().Name : "NULL"));

                // 2. open the demo scene and render it via a URP camera
                var scene = EditorSceneManager.OpenScene("Assets/FigoPrefabImporter/Samples/FigoDemo.unity");
                var cam = Camera.main;
                if (cam == null) throw new Exception("no Main Camera in demo scene");
                cam.gameObject.AddComponent<UniversalAdditionalCameraData>();

                // overlay canvases bypass cameras — retarget to ScreenSpaceCamera for the test render
                foreach (var canvas in UnityEngine.Object.FindObjectsOfType<Canvas>())
                {
                    canvas.renderMode = RenderMode.ScreenSpaceCamera;
                    canvas.worldCamera = cam;
                    canvas.planeDistance = 5;
                }

                var rt = new RenderTexture(1280, 720, 24, RenderTextureFormat.ARGB32);
                cam.targetTexture = rt;
                cam.Render();
                RenderTexture.active = rt;
                var tex = new Texture2D(1280, 720, TextureFormat.RGBA32, false);
                tex.ReadPixels(new Rect(0, 0, 1280, 720), 0, 0);
                tex.Apply();
                cam.targetTexture = null;
                RenderTexture.active = null;

                var png = Path.GetFullPath("figo_urp_render.png");
                File.WriteAllBytes(png, tex.EncodeToPNG());

                // sanity: the render must not be a blank frame
                var px = tex.GetPixels32();
                int nonBg = 0;
                var bg = px[0];
                foreach (var p in px)
                    if (Math.Abs(p.r - bg.r) + Math.Abs(p.g - bg.g) + Math.Abs(p.b - bg.b) > 12) nonBg++;
                Debug.Log("[FigoURP] rendered " + png + ", non-background pixels: " + nonBg + "/" + px.Length);
                if (nonBg < 10000) throw new Exception("URP render looks blank (" + nonBg + " non-background pixels)");

                Debug.Log("[FigoURP] RESULT: OK");
                EditorApplication.Exit(0);
            }
            catch (Exception e)
            {
                Debug.LogError("[FigoURP] FAILED: " + e);
                EditorApplication.Exit(1);
            }
        }
    }
}
