(function () {
    Module.locateFile = function (path) {
        return path + "?t=" + Date.now();
    };
    window.imageCanvasFetchFont = function () {
        var url = "/assets/WenQuanYiMicroHei.ttf";
        var path = "/assets/WenQuanYiMicroHei.ttf";

        try { FS.mkdir("/assets"); } catch (error) {}

        fetch(url).then(function (response) {
            if (!response.ok) throw new Error("font HTTP " + response.status);
            return response.arrayBuffer();
        }).then(function (data) {
            FS.writeFile(path, new Uint8Array(data));
            Module.ccall("WebFontReady", null, ["number"], [1]);
        }).catch(function (error) {
            console.error("font fetch failed", error);
            Module.ccall("WebFontReady", null, ["number"], [0]);
        });
    };
})();

(function () {
    window.__desktopMousePressed = [0, 0, 0];
    window.__desktopMouseReleased = [0, 0, 0];
    window.__desktopMouseClicked = [0, 0, 0];
    function mouseIndex(button) {
        if (button === 1) return 2;
        if (button === 2) return 1;
        return button;
    }
    function markPressed(event) {
        var index = mouseIndex(event.button);
        if (index >= 0 && index < 3)
            window.__desktopMousePressed[index] = 1;
    }
    function markReleased(event) {
        var index = mouseIndex(event.button);
        if (index >= 0 && index < 3)
            window.__desktopMouseReleased[index] = 1;
    }
    document.addEventListener("pointerdown", markPressed, true);
    document.addEventListener("mousedown", markPressed, true);
    document.addEventListener("pointerup", markReleased, true);
    document.addEventListener("mouseup", markReleased, true);
    document.addEventListener("click", function (event) {
        var index = mouseIndex(event.button);
        if (index >= 0 && index < 3)
            window.__desktopMouseClicked[index] = 1;
    }, true);
    window.__desktopWheel = 0;
    document.addEventListener("wheel", function (event) {
        if (event.target !== document.getElementById("canvas")) return;
        window.__desktopWheel += event.deltaY < 0 ? 1 : (event.deltaY > 0 ? -1 : 0);
        event.preventDefault();
    }, { capture: true, passive: false });
})();

(function () {
    window.__desktopKeyEvents = [];
    window.__desktopChars = [];
    window.__desktopKeyState = {};
    function keyCode(event) {
        switch (event.key) {
            case "Backspace": return 259;
            case "Tab": return 258;
            case "Enter": return 257;
            case "Escape": return 256;
            case "Delete": return 261;
            case "Insert": return 260;
            case "Home": return 268;
            case "End": return 269;
            case "PageUp": return 266;
            case "PageDown": return 267;
            case "ArrowLeft": return 263;
            case "ArrowRight": return 262;
            case "ArrowDown": return 264;
            case "ArrowUp": return 265;
            case "Control": return event.location === 2 ? 345 : 341;
            case "Shift": return event.location === 2 ? 344 : 340;
            case "Alt": return event.location === 2 ? 343 : 342;
            case "Meta": return event.location === 2 ? 348 : 347;
            default: return 0;
        }
    }
    function setControlState(event, down) {
        if (event.key !== "Control") return;
        window.__desktopKeyState[341] = down;
        window.__desktopKeyState[345] = down;
    }
    function keyDown(event) {
        var code = keyCode(event);
        if (code) {
            window.__desktopKeyEvents.push(code);
            window.__desktopKeyState[code] = 1;
            event.preventDefault();
        }
        setControlState(event, 1);
        if (event.key.length === 1 && !event.ctrlKey && !event.metaKey &&
            !event.altKey && !event.isComposing) {
            window.__desktopChars.push(event.key.codePointAt(0));
        }
    }
    function keyUp(event) {
        var code = keyCode(event);
        if (code) window.__desktopKeyState[code] = 0;
        setControlState(event, 0);
    }
    document.addEventListener("keydown", keyDown, true);
    document.addEventListener("keyup", keyUp, true);
    document.addEventListener("compositionend", function (event) {
        for (var i = 0; i < event.data.length; i++)
            window.__desktopChars.push(event.data.codePointAt(i));
    }, true);
    window.addEventListener("blur", function () {
        window.__desktopKeyState = {};
    });
})();
