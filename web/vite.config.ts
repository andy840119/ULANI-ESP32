import { defineConfig } from 'vite';

// Fixed asset names: the firmware embeds exactly index.html.gz, app.js.gz and
// app.css.gz, so content hashing would break the build.
export default defineConfig({
  base: '/',
  build: {
    target: 'es2020',
    assetsInlineLimit: 0,
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        chunkFileNames: 'app.js',
        assetFileNames: 'app.[ext]',
      },
    },
  },
  server: {
    // `npm run dev` proxies the API to a board on the default SoftAP address.
    proxy: {
      '/api': { target: 'http://192.168.4.1', changeOrigin: true },
    },
  },
});
