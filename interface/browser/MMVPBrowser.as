package
{
    import flash.display.DisplayObject;
    import flash.display.Loader;
    import flash.display.Sprite;
    import flash.display.StageAlign;
    import flash.display.StageScaleMode;
    import flash.events.Event;
    import flash.events.IOErrorEvent;
    import flash.events.KeyboardEvent;
    import flash.events.MouseEvent;
    import flash.geom.Rectangle;
    import flash.net.URLRequest;
    import flash.system.ApplicationDomain;
    import flash.system.LoaderContext;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFormat;
    import flash.ui.Keyboard;

    [SWF(width="826", height="700", frameRate="60", backgroundColor="#061108")]
    public final class MMVPBrowser extends Sprite
    {
        // F4SE attaches its per-movie API object as root.f4se. This document
        // class is sealed, so the slot must be declared or SetMember cannot
        // expose f4se.plugins.MainMenuVideoPlayer to ActionScript.
        public var f4se:Object;
        public var BGSCodeObj:Object = {};
        public var IsMiniGame:Boolean = true;
        public var UseOwnCursor:Boolean = true;

        private const rootActions:Array = [
            { id: "movies", label: "MOVIES" },
            { id: "television", label: "TELEVISION" },
            { id: "random", label: "RANDOM" },
            { id: "continue", label: "CONTINUE WATCHING" }
        ];
        private const pageSize:int = 4;

        private var bridge:MMVPBridge;
        private var browserLayer:Sprite;
        private var titleField:TextField;
        private var statusField:TextField;
        private var hintField:TextField;
        private var cursorSprite:Sprite;
        private var buttons:Array = [];
        private var selectedIndex:int = 0;
        private var viewMode:String = "root";
        private var mediaChannel:int = 0;
        private var mediaItems:Array = [];
        private var mediaOffset:int = 0;
        private var playerLoader:Loader;
        private var playerContent:Object;
        private var platform:uint = 0;
        private var psnButtonSwap:Boolean = false;
        private var paused:Boolean = false;
        private var fontReady:Boolean = false;

        public function MMVPBrowser()
        {
            bridge = new MMVPBridge(this);
            addEventListener(Event.ADDED_TO_STAGE, onAddedToStage);
            addEventListener(Event.REMOVED_FROM_STAGE, onRemovedFromStage);
        }

        public function InitProgram():void
        {
            setInterfaceActive(true);
            updateBridgeStatus();
        }

        public function ProcessUserEvent(
            eventName:String,
            pressed:Boolean):Boolean
        {
            if (!pressed) {
                return false;
            }

            if (playerContent != null &&
                playerContent["ProcessUserEvent"] is Function) {
                return Boolean(
                    playerContent["ProcessUserEvent"](eventName, pressed));
            }

            var normalized:String = eventName.toLowerCase();
            if (normalized == "up") {
                moveSelection(-1);
                return true;
            }
            if (normalized == "down") {
                moveSelection(1);
                return true;
            }
            if (normalized == "left" ||
                normalized == "jump") {
                if (normalized == "left" && inListView()) {
                    changePage(-1);
                }
                return true;
            }
            if (normalized == "right") {
                if (inListView()) {
                    changePage(1);
                }
                return true;
            }
            if (normalized == "accept" || normalized == "activate") {
                activateSelection();
                return true;
            }
            if (normalized == "cancel" || normalized == "back") {
                handleBack();
                return true;
            }
            // A holotape program owns input while it is active. Returning
            // false here lets mapped gameplay actions leak back into Fallout.
            return true;
        }

        public function Pause(value:Boolean):void
        {
            paused = value;
            if (playerContent != null && playerContent["Pause"] is Function) {
                playerContent["Pause"](value);
            }
        }

        public function SetPlatform(value:uint, swap:Boolean):void
        {
            platform = value;
            psnButtonSwap = swap;
            if (playerContent != null &&
                playerContent["SetPlatform"] is Function) {
                playerContent["SetPlatform"](value, swap);
            }
        }

        public function onCodeObjDestruction():void
        {
            setInterfaceActive(false);
            unloadPlayer();
            BGSCodeObj = null;
        }

        public function ClosePlayer():void
        {
            unloadPlayer();
            draw();
            updateBridgeStatus();
        }

        public function CallNative(name:String, arguments:Array = null):*
        {
            return bridge.call(name, arguments);
        }

        private function onAddedToStage(event:Event):void
        {
            stage.scaleMode = StageScaleMode.NO_SCALE;
            stage.align = StageAlign.TOP_LEFT;
            stage.frameRate = 60.0;
            stage.addEventListener(Event.RESIZE, onResize);
            stage.addEventListener(Event.ENTER_FRAME, onEnterFrame);
            stage.addEventListener(KeyboardEvent.KEY_DOWN, onKeyDown);
            stage.addEventListener(MouseEvent.MOUSE_MOVE, onMouseMove);
            setInterfaceActive(true);
            MMVPFontLibrary.load(onFontReady);
        }

        private function onFontReady(success:Boolean):void
        {
            fontReady = success;
            setInterfaceActive(true);
            createInterface();
            draw();
            updateBridgeStatus();
        }

        private function onRemovedFromStage(event:Event):void
        {
            setInterfaceActive(false);
            if (stage != null) {
                stage.removeEventListener(Event.RESIZE, onResize);
                stage.removeEventListener(Event.ENTER_FRAME, onEnterFrame);
                stage.removeEventListener(KeyboardEvent.KEY_DOWN, onKeyDown);
                stage.removeEventListener(MouseEvent.MOUSE_MOVE, onMouseMove);
            }
            unloadPlayer();
        }

        private function createInterface():void
        {
            if (titleField != null) {
                return;
            }

            browserLayer = new Sprite();
            addChild(browserLayer);

            titleField = makeText(31, 0x9CFFAE, true);
            titleField.text = "MAIN MENU VIDEO PLAYER";
            browserLayer.addChild(titleField);

            statusField = makeText(18, 0x74C983, false);
            browserLayer.addChild(statusField);

            hintField = makeText(16, 0x74C983, false);
            hintField.text = "ACCEPT: SELECT     BACK: CLOSE";
            browserLayer.addChild(hintField);

            for (var index:int = 0; index < pageSize; ++index) {
                var button:Sprite = new Sprite();
                button.name = String(index);
                button.buttonMode = true;
                button.mouseChildren = false;
                button.addEventListener(MouseEvent.CLICK, onButtonClick);
                button.addEventListener(MouseEvent.MOUSE_OVER, onButtonOver);

                var label:TextField = makeText(24, 0x9CFFAE, true);
                label.name = "label";
                button.addChild(label);
                browserLayer.addChild(button);
                buttons.push(button);
            }
            syncNativeSelection();

            cursorSprite = new Sprite();
            cursorSprite.mouseEnabled = false;
            cursorSprite.mouseChildren = false;
            drawCursor(cursorSprite);
            cursorSprite.x = stage.stageWidth * 0.5;
            cursorSprite.y = stage.stageHeight * 0.5;
            addChild(cursorSprite);
        }

        private function draw():void
        {
            if (stage == null || titleField == null || playerLoader != null) {
                return;
            }

            browserLayer.visible = true;
            var width:Number = Math.max(stage.stageWidth, 1);
            var height:Number = Math.max(stage.stageHeight, 1);
            var safe:Rectangle = MMVPLayout.safeBounds(width, height);

            browserLayer.graphics.clear();
            browserLayer.graphics.beginFill(0x061108, 1.0);
            browserLayer.graphics.drawRect(0, 0, width, height);
            browserLayer.graphics.endFill();
            browserLayer.graphics.lineStyle(2, 0x5AA66A, 0.85);
            browserLayer.graphics.drawRect(
                safe.x,
                safe.y,
                safe.width,
                safe.height);

            titleField.x = safe.x + 20;
            titleField.y = safe.y + 16;
            statusField.x = safe.x + 22;
            statusField.y = safe.y + 63;
            hintField.x = safe.x + 20;
            hintField.y = safe.bottom - hintField.height - 17;

            var top:Number = safe.y + 112;
            var gap:Number = 14;
            var available:Number =
                safe.height - 112 - hintField.height - 44;
            var buttonHeight:Number =
                Math.max(56, (available - gap * (pageSize - 1)) /
                    pageSize);

            for (var index:int = 0; index < buttons.length; ++index) {
                var button:Sprite = buttons[index] as Sprite;
                var item:Object = visibleItem(index);
                button.visible = item != null;
                if (item == null) {
                    continue;
                }
                var label:TextField =
                    button.getChildByName("label") as TextField;
                label.text = formatItemLabel(item);
                button.x = safe.x + 20;
                button.y = top + index * (buttonHeight + gap);
                drawButton(
                    button,
                    safe.width - 40,
                    buttonHeight,
                    index == selectedIndex);
            }

            if (inListView()) {
                var first:int = mediaItems.length == 0
                    ? 0
                    : mediaOffset + 1;
                var last:int = Math.min(
                    mediaOffset + pageSize,
                    mediaItems.length);
                titleField.text = viewMode == "continue"
                    ? "CONTINUE WATCHING"
                    : mediaChannel == 1
                        ? "MOVIES"
                        : "TELEVISION";
                statusField.text = mediaItems.length == 0
                    ? "NO MEDIA FOUND"
                    : "ITEMS " + first + "-" + last +
                        " OF " + mediaItems.length;
                hintField.text =
                    "ACCEPT: PLAY   LEFT/RIGHT: PAGE   BACK: CATEGORIES";
            } else {
                titleField.text = "MAIN MENU VIDEO PLAYER";
                hintField.text = "ACCEPT: SELECT     BACK: CLOSE";
            }
        }

        private function drawButton(
            button:Sprite,
            width:Number,
            height:Number,
            selected:Boolean):void
        {
            button.graphics.clear();
            button.graphics.lineStyle(
                selected ? 3 : 1,
                selected ? 0xB9FFC4 : 0x477C50,
                1.0);
            button.graphics.beginFill(
                selected ? 0x173C20 : 0x0B2110,
                0.95);
            button.graphics.drawRect(0, 0, width, height);
            button.graphics.endFill();

            var label:TextField = button.getChildByName("label") as TextField;
            label.textColor = selected ? 0xD2FFD8 : 0x8DE29B;
            label.x = 20;
            label.y = (height - label.height) * 0.5 - 2;
        }

        private function updateBridgeStatus():void
        {
            if (statusField == null) {
                return;
            }
            statusField.text = bridge.available
                ? "NATIVE BRIDGE READY  /  API " + bridge.apiVersion
                : fontReady
                    ? "UI PROTOTYPE  /  NATIVE BRIDGE PENDING"
                    : "PROGRAM FONT FAILED TO LOAD";
        }

        private function moveSelection(delta:int):void
        {
            if (inListView() && mediaItems.length > 0) {
                var previousOffset:int = mediaOffset;
                var absoluteIndex:int =
                    mediaOffset + selectedIndex;
                absoluteIndex =
                    (absoluteIndex + delta +
                     mediaItems.length * (Math.abs(delta) + 1)) %
                    mediaItems.length;
                mediaOffset =
                    int(absoluteIndex / pageSize) * pageSize;
                selectedIndex = absoluteIndex - mediaOffset;
                if (mediaOffset != previousOffset) {
                    refreshVisibleProgress();
                }
                syncNativeSelection();
                draw();
                return;
            }
            var count:int = visibleCount();
            if (count <= 0) {
                return;
            }
            selectedIndex =
                (selectedIndex + delta + count) % count;
            syncNativeSelection();
            draw();
        }

        private function activateSelection():void
        {
            var item:Object = visibleItem(selectedIndex);
            if (item == null) {
                return;
            }
            if (inListView()) {
                var accepted:* = bridge.call(
                    "playMedia",
                    [int(item.channel), String(item.id)]);
                statusField.text = accepted === true
                    ? "STARTING " + String(item.label) + "..."
                    : "MEDIA COULD NOT BE STARTED";
                return;
            }

            var actionId:String = String(item.id);
            if (actionId == "movies") {
                openMediaList(1);
            } else if (actionId == "television") {
                openMediaList(2);
            } else if (actionId == "continue") {
                openContinueList();
            } else {
                startNativePlayback(actionId);
            }
        }

        private function onButtonClick(event:MouseEvent):void
        {
            selectedIndex = int(DisplayObject(event.currentTarget).name);
            syncNativeSelection();
            activateSelection();
        }

        private function onButtonOver(event:MouseEvent):void
        {
            selectedIndex = int(DisplayObject(event.currentTarget).name);
            syncNativeSelection();
            draw();
        }

        private function syncNativeSelection():void
        {
            bridge.call(
                "setSelection",
                [selectedIndex, inListView() ? 1 : 0]);
        }

        private function onEnterFrame(event:Event):void
        {
            var navigation:* =
                bridge.call("consumeNavigationRequest");
            if (navigation is Number ||
                navigation is int ||
                navigation is uint) {
                var navigationValue:int = int(navigation);
                if (navigationValue >= 10 ||
                    navigationValue <= -10) {
                    changePage(navigationValue > 0 ? 1 : -1);
                } else if (navigationValue != 0) {
                    moveSelection(navigationValue);
                }
            }
            var accept:* = bridge.call("consumeAcceptRequest");
            if (accept is Number || accept is int || accept is uint) {
                var requestedIndex:int = int(accept);
                var count:int = visibleCount();
                if (requestedIndex >= 0 &&
                    requestedIndex < count) {
                    selectedIndex = requestedIndex;
                    syncNativeSelection();
                    draw();
                    activateSelection();
                }
            }
            if (bridge.call("consumeBackRequest") === true) {
                handleBack();
            }
            if (bridge.call("consumeProgressRefresh") === true) {
                refreshVisibleProgress();
                draw();
            }
        }

        private function openMediaList(channel:int):void
        {
            var countValue:* = bridge.call("refreshMedia", [channel]);
            if (!(countValue is Number ||
                  countValue is int ||
                  countValue is uint)) {
                statusField.text = "MEDIA CATALOG BRIDGE UNAVAILABLE";
                return;
            }

            var count:int = Math.max(0, Math.min(int(countValue), 10000));
            var items:Array = [];
            for (var index:int = 0; index < count; ++index) {
                var id:* = bridge.call("getMediaId", [channel, index]);
                var label:* =
                    bridge.call("getMediaLabel", [channel, index]);
                if (id is String && String(id).length > 0) {
                    items.push({
                        id: String(id),
                        channel: channel,
                        label: label is String && String(label).length > 0
                            ? String(label)
                            : "UNTITLED MEDIA",
                        progress: -1
                    });
                }
            }

            mediaChannel = channel;
            mediaItems = items;
            mediaOffset = 0;
            selectedIndex = 0;
            viewMode = "media";
            refreshVisibleProgress();
            syncNativeSelection();
            draw();
        }

        private function openContinueList():void
        {
            var countValue:* = bridge.call("refreshContinue");
            if (!(countValue is Number ||
                  countValue is int ||
                  countValue is uint)) {
                statusField.text = "CONTINUE BRIDGE UNAVAILABLE";
                return;
            }

            var count:int = Math.max(0, Math.min(int(countValue), 10000));
            var items:Array = [];
            for (var index:int = 0; index < count; ++index) {
                var channel:* =
                    bridge.call("getContinueChannel", [index]);
                var id:* = bridge.call("getContinueId", [index]);
                var label:* =
                    bridge.call("getContinueLabel", [index]);
                if ((channel is Number ||
                     channel is int ||
                     channel is uint) &&
                    int(channel) >= 1 &&
                    int(channel) <= 2 &&
                    id is String &&
                    String(id).length > 0) {
                    items.push({
                        id: String(id),
                        channel: int(channel),
                        label: label is String &&
                               String(label).length > 0
                            ? String(label)
                            : "UNTITLED MEDIA",
                        progress: -1
                    });
                }
            }

            mediaChannel = 0;
            mediaItems = items;
            mediaOffset = 0;
            selectedIndex = 0;
            viewMode = "continue";
            refreshVisibleProgress();
            syncNativeSelection();
            draw();
        }

        private function showRoot():void
        {
            viewMode = "root";
            mediaChannel = 0;
            mediaItems = [];
            mediaOffset = 0;
            selectedIndex = 0;
            statusField.text = bridge.available
                ? "NATIVE BRIDGE READY  /  API " + bridge.apiVersion
                : "NATIVE BRIDGE UNAVAILABLE";
            syncNativeSelection();
            draw();
        }

        private function handleBack():void
        {
            if (inListView()) {
                showRoot();
            } else {
                closeHolotape();
            }
        }

        private function changePage(delta:int):void
        {
            if (!inListView() || mediaItems.length <= pageSize) {
                return;
            }
            var pageCount:int =
                Math.ceil(mediaItems.length / Number(pageSize));
            var page:int = int(mediaOffset / pageSize);
            page = (page + delta + pageCount) % pageCount;
            mediaOffset = page * pageSize;
            selectedIndex = 0;
            refreshVisibleProgress();
            syncNativeSelection();
            draw();
        }

        private function visibleCount():int
        {
            return inListView()
                ? Math.max(
                    0,
                    Math.min(pageSize, mediaItems.length - mediaOffset))
                : rootActions.length;
        }

        private function visibleItem(index:int):Object
        {
            if (index < 0 || index >= visibleCount()) {
                return null;
            }
            return inListView()
                ? mediaItems[mediaOffset + index]
                : rootActions[index];
        }

        private static function shortenLabel(value:String):String
        {
            const maximum:int = 47;
            return value.length <= maximum
                ? value
                : value.substr(0, maximum - 3) + "...";
        }

        private function formatItemLabel(item:Object):String
        {
            var value:String = String(item.label);
            if (!inListView()) {
                return shortenLabel(value);
            }

            var percentage:int = item.progress == null
                ? -1
                : int(item.progress);
            var suffix:String = percentage < 0
                ? "  [NEW]"
                : percentage >= 100
                    ? "  [WATCHED]"
                    : "  [" + percentage + "%]";
            const maximum:int = 47;
            var available:int = Math.max(3, maximum - suffix.length);
            if (value.length > available) {
                value = value.substr(0, available - 3) + "...";
            }
            return value + suffix;
        }

        private function refreshVisibleProgress():void
        {
            if (!inListView()) {
                return;
            }
            var last:int = Math.min(
                mediaOffset + pageSize,
                mediaItems.length);
            for (var index:int = mediaOffset; index < last; ++index) {
                var item:Object = mediaItems[index];
                var value:* = bridge.call(
                    "getMediaProgress",
                    [int(item.channel), String(item.id)]);
                item.progress =
                    value is Number || value is int || value is uint
                        ? int(value)
                        : -1;
            }
        }

        private function inListView():Boolean
        {
            return viewMode == "media" || viewMode == "continue";
        }

        private function startNativePlayback(actionId:String):void
        {
            var command:int = 3;
            if (actionId == "movies") {
                command = 1;
            } else if (actionId == "television") {
                command = 2;
            } else if (actionId == "random") {
                command = Math.random() < 0.5 ? 1 : 2;
            }

            var accepted:* = bridge.call("playCommand", [command]);
            if (accepted !== true &&
                !bridge.setValue("command", command)) {
                statusField.text = "NATIVE BRIDGE UNAVAILABLE";
                return;
            }
            statusField.text = "STARTING VIDEO...";
        }

        private function setInterfaceActive(value:Boolean):void
        {
            bridge.setValue("interfaceActive", value);
        }

        private function openPlayer(actionId:String):void
        {
            if (playerLoader != null) {
                return;
            }

            playerLoader = new Loader();
            playerLoader.contentLoaderInfo.addEventListener(
                Event.COMPLETE,
                onPlayerLoaded);
            playerLoader.contentLoaderInfo.addEventListener(
                IOErrorEvent.IO_ERROR,
                onPlayerLoadError);
            playerLoader.name = actionId;
            addChild(playerLoader);
            setChildIndex(cursorSprite, numChildren - 1);
            browserLayer.visible = false;

            var context:LoaderContext = new LoaderContext(
                false,
                new ApplicationDomain(ApplicationDomain.currentDomain));
            playerLoader.load(new URLRequest("MMVPPlayer.swf"), context);
        }

        private function onPlayerLoaded(event:Event):void
        {
            playerContent = playerLoader.content;
            if (playerContent == null) {
                return;
            }
            playerContent["Host"] = this;
            playerContent["BGSCodeObj"] = BGSCodeObj;
            if (playerContent["SetPlatform"] is Function) {
                playerContent["SetPlatform"](platform, psnButtonSwap);
            }
            if (playerContent["SetLaunchContext"] is Function) {
                playerContent["SetLaunchContext"]({
                    action: playerLoader.name,
                    target: MMVPTypes.pipBoyTarget()
                });
            }
            if (playerContent["Pause"] is Function) {
                playerContent["Pause"](paused);
            }
            if (playerContent["InitProgram"] is Function) {
                playerContent["InitProgram"]();
            }
        }

        private function onPlayerLoadError(event:IOErrorEvent):void
        {
            unloadPlayer();
            statusField.text = "PLAYER LOAD FAILED: " + event.text;
            draw();
        }

        private function unloadPlayer():void
        {
            playerContent = null;
            if (playerLoader == null) {
                if (browserLayer != null) {
                    browserLayer.visible = true;
                }
                return;
            }

            try {
                playerLoader.unloadAndStop(true);
            } catch (error:Error) {
                try {
                    playerLoader.unload();
                } catch (ignored:Error) {
                }
            }
            if (contains(playerLoader)) {
                removeChild(playerLoader);
            }
            playerLoader = null;
            browserLayer.visible = true;
        }

        private function closeHolotape():void
        {
            if (bridge.hasMethod("closeProgram")) {
                bridge.call("closeProgram");
                return;
            }
            if (BGSCodeObj != null &&
                BGSCodeObj["closeHolotape"] is Function) {
                BGSCodeObj["closeHolotape"]();
            }
        }

        private function onResize(event:Event):void
        {
            draw();
            if (playerContent != null &&
                playerContent["HostResized"] is Function) {
                playerContent["HostResized"]();
            }
        }

        private function onMouseMove(event:MouseEvent):void
        {
            if (cursorSprite == null) {
                return;
            }
            cursorSprite.x = Math.max(
                0,
                Math.min(stage.stageWidth - 1, event.stageX));
            cursorSprite.y = Math.max(
                0,
                Math.min(stage.stageHeight - 1, event.stageY));
            for (var index:int = 0; index < buttons.length; ++index) {
                var button:Sprite = buttons[index] as Sprite;
                if (button.hitTestPoint(
                        event.stageX,
                        event.stageY,
                        true)) {
                    if (selectedIndex != index) {
                        selectedIndex = index;
                        syncNativeSelection();
                        draw();
                    }
                    break;
                }
            }
            if (cursorSprite.parent == this) {
                setChildIndex(cursorSprite, numChildren - 1);
            }
            event.updateAfterEvent();
        }

        private function onKeyDown(event:KeyboardEvent):void
        {
            if (event.keyCode == Keyboard.TAB) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Cancel", true);
            } else if (event.keyCode == Keyboard.UP ||
                       event.keyCode == 87) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Up", true);
            } else if (event.keyCode == Keyboard.DOWN ||
                       event.keyCode == 83) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Down", true);
            } else if (event.keyCode == Keyboard.LEFT ||
                       event.keyCode == 65) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Left", true);
            } else if (event.keyCode == Keyboard.RIGHT ||
                       event.keyCode == 68) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Right", true);
            } else if (event.keyCode == Keyboard.ENTER ||
                       event.keyCode == Keyboard.SPACE) {
                event.stopImmediatePropagation();
                ProcessUserEvent("Accept", true);
            } else {
                // Escape, Backspace, and other gameplay keys are deliberately
                // inert. Tab is the only keyboard exit/back action.
                event.stopImmediatePropagation();
            }
        }

        private static function drawCursor(cursor:Sprite):void
        {
            cursor.graphics.clear();
            cursor.graphics.lineStyle(2, 0xD5FFDA, 1.0);
            cursor.graphics.beginFill(0x42FF5E, 0.95);
            cursor.graphics.moveTo(0, 0);
            cursor.graphics.lineTo(0, 25);
            cursor.graphics.lineTo(7, 18);
            cursor.graphics.lineTo(13, 29);
            cursor.graphics.lineTo(18, 26);
            cursor.graphics.lineTo(12, 15);
            cursor.graphics.lineTo(23, 14);
            cursor.graphics.lineTo(0, 0);
            cursor.graphics.endFill();
        }

        private static function makeText(
            size:Number,
            color:uint,
            bold:Boolean):TextField
        {
            var field:TextField = new TextField();
            field.autoSize = TextFieldAutoSize.LEFT;
            field.selectable = false;
            field.mouseEnabled = false;
            field.embedFonts = true;
            field.defaultTextFormat = new TextFormat(
                MMVPFontLibrary.FONT_NAME,
                size,
                color,
                bold);
            return field;
        }
    }
}
