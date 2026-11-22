import { defineConfig } from "vite";

// Tauri 2 loads `dist/` from disk, so asset URLs stay relative (`base: "./"`).
// The dev server port is fixed so `devUrl` in tauri.conf.json can point at it.
export default defineConfig({
  base: "./",
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
  },
  build: {
    target: "es2022",
    outDir: "dist",
    emptyOutDir: true,
    sourcemap: false,
    cssCodeSplit: true,
    // The mock backend is its own chunk and only loads outside Tauri.
    chunkSizeWarningLimit: 300,
  },
});
