import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

const apiTarget = process.env.VITE_PROXY_TARGET ?? "http://127.0.0.1:18081";

export default defineConfig({
  plugins: [solid()],
  server: {
    host: "127.0.0.1",
    port: 5173,
    proxy: {
      "/api": {
        target: apiTarget,
        changeOrigin: true,
        ws: true,
        rewrite: (path) => path.replace(/^\/api/, ""),
      },
    },
  },
  build: {
    target: "es2022",
    sourcemap: true,
  },
});
