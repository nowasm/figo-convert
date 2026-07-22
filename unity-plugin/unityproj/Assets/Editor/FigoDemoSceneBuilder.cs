// Builds the shipped demo scene (Samples/FigoDemo.unity) from the
// pre-converted Starfall prefabs. Dev-only tool — lives outside
// Assets/FigoPrefabImporter so it is not part of the package.
// Run: Unity -batchmode -projectPath ... -executeMethod Figo.PrefabImporter.FigoDemoSceneBuilder.Build -quit
using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace Figo.PrefabImporter
{
    public static class FigoDemoSceneBuilder
    {
        const string ScenePath = "Assets/FigoPrefabImporter/Samples/FigoDemo.unity";
        const string MenuPrefab = "Assets/FigoPrefabImporter/Samples/StarfallPrefabs/menu.prefab";
        const string SettingsPrefab = "Assets/FigoPrefabImporter/Samples/StarfallPrefabs/settings.prefab";

        public static void Build()
        {
            try
            {
                var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

                // camera (solid background so the scene runs standalone too)
                var camGo = new GameObject("Main Camera");
                var cam = camGo.AddComponent<Camera>();
                camGo.tag = "MainCamera";
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = new Color(0.03f, 0.04f, 0.08f);
                cam.orthographic = true;
                camGo.transform.position = new Vector3(0, 0, -10);

                // canvas sized to the design (1280x720)
                var canvasGo = new GameObject("Canvas");
                var canvas = canvasGo.AddComponent<Canvas>();
                canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                var scaler = canvasGo.AddComponent<CanvasScaler>();
                scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
                scaler.referenceResolution = new Vector2(1280, 720);
                scaler.matchWidthOrHeight = 0.5f;
                canvasGo.AddComponent<GraphicRaycaster>();

                var evGo = new GameObject("EventSystem");
                evGo.AddComponent<EventSystem>();
                evGo.AddComponent<StandaloneInputModule>();

                // both converted screens as prefab instances: menu on, settings off
                InstantiateScreen(MenuPrefab, canvasGo.transform, true);
                InstantiateScreen(SettingsPrefab, canvasGo.transform, false)
                    .name += " (disable menu, enable this to view)";

                EditorSceneManager.SaveScene(scene, ScenePath);
                AssetDatabase.Refresh();
                if (!File.Exists(ScenePath)) throw new Exception("scene was not saved: " + ScenePath);
                Debug.Log("[FigoDemoScene] RESULT: OK -> " + ScenePath);
                if (Application.isBatchMode) EditorApplication.Exit(0);
            }
            catch (Exception e)
            {
                Debug.LogError("[FigoDemoScene] FAILED: " + e);
                if (Application.isBatchMode) EditorApplication.Exit(1);
            }
        }

        static GameObject InstantiateScreen(string prefabPath, Transform parent, bool active)
        {
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
            if (prefab == null) throw new Exception("prefab not found: " + prefabPath);
            var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, parent);
            var rt = go.GetComponent<RectTransform>();
            if (rt != null)
            {
                rt.anchoredPosition = Vector2.zero;
                rt.localScale = Vector3.one;
            }
            go.SetActive(active);
            return go;
        }
    }
}
