(function () {
    window.imageCanvasFetchFont = function () {
        var url = "/assets/WenQuanYiMicroHei.ttf";
        var path = "/assets/WenQuanYiMicroHei.ttf";
        var cacheName = "image-canvas-font-v1";

        try { FS.mkdir("/assets"); } catch (error) {}

        function finish(ok) {
            Module.ccall("WebFontReady", null, ["number"], [ok ? 1 : 0]);
        }

        function writeFont(data) {
            try {
                FS.writeFile(path, new Uint8Array(data));
                finish(true);
            } catch (error) {
                console.error("font write failed", error);
                finish(false);
            }
        }

        function fetchFromNetwork() {
            fetch(url).then(function (response) {
                if (!response.ok) throw new Error("font HTTP " + response.status);
                return response.arrayBuffer();
            }).then(function (data) {
                writeFont(data);
                if ("caches" in window) {
                    caches.open(cacheName).then(function (cache) {
                        cache.put(url, new Response(data)).catch(function () {});
                    }).catch(function () {});
                }
            }).catch(function (error) {
                console.error("font fetch failed", error);
                finish(false);
            });
        }

        if ("caches" in window) {
            caches.open(cacheName).then(function (cache) {
                return cache.match(url).then(function (cached) {
                    if (cached) return cached.arrayBuffer().then(writeFont);
                    fetchFromNetwork();
                });
            }).catch(fetchFromNetwork);
        } else {
            fetchFromNetwork();
        }
    };
})();
