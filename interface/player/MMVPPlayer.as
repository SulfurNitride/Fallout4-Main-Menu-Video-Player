package
{
    import flash.display.DisplayObject;
    import flash.display.Loader;
    import flash.display.Sprite;
    import flash.display.StageAlign;
    import flash.display.StageScaleMode;
    import flash.events.Event;
    import flash.events.IOErrorEvent;
    import flash.events.MouseEvent;
    import flash.geom.Rectangle;
    import flash.net.URLRequest;
    import flash.system.ApplicationDomain;
    import flash.system.LoaderContext;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFormat;

    [SWF(width="826", height="700", frameRate="60", backgroundColor="#020603")]
    public final class MMVPPlayer extends Sprite
    {
        public var Host:Object;
        public var BGSCodeObj:Object = {};
        public var IsMiniGame:Boolean = true;
        public var UseOwnCursor:Boolean = true;

        private const controls:Array = [
            { id: "previous", label: "|<" },
            { id: "pause", label: "PAUSE" },
            { id: "next", label: ">|" },
            { id: "stop", label: "STOP" },
            { id: "back", label: "BACK" }
        ];

        private var videoFrame:Sprite;
        private var videoLoader:Loader;
        private var titleField:TextField;
        private var statusField:TextField;
        private var timeField:TextField;
        private var buttons:Array = [];
        private var selectedIndex:int = 1;
        private var launchContext:Object;
        private var state:Object = {};
        private var isPaused:Boolean = false;

        public function MMVPPlayer()
        {
            addEventListener(Event.ADDED_TO_STAGE, onAddedToStage);
            addEventListener(Event.REMOVED_FROM_STAGE, onRemovedFromStage);
        }

        public function InitProgram():void
        {
            updateStatus();
        }

        public function SetLaunchContext(value:Object):void
        {
            launchContext = value;
            updateStatus();
        }

        public function UpdateState(value:Object):void
        {
            state = value == null ? {} : value;
            updateStatus();
            draw();
        }

        public function onVideoTextureReady():void
        {
            loadExternalVideo();
        }

        public function HostResized():void
        {
            draw();
        }

        public function ProcessUserEvent(
            eventName:String,
            pressed:Boolean):Boolean
        {
            if (!pressed) {
                return false;
            }

            var normalized:String = eventName.toLowerCase();
            if (normalized == "left") {
                moveSelection(-1);
                return true;
            }
            if (normalized == "right") {
                moveSelection(1);
                return true;
            }
            if (normalized == "accept" || normalized == "activate") {
                activateSelection();
                return true;
            }
            if (normalized == "cancel" || normalized == "back") {
                closePlayer();
                return true;
            }
            // Keep gameplay actions from falling through while the player
            // child owns the holotape program.
            return true;
        }

        public function Pause(value:Boolean):void
        {
            isPaused = value;
            updateStatus();
        }

        public function SetPlatform(platform:uint, swap:Boolean):void
        {
        }

        public function onCodeObjDestruction():void
        {
            unloadExternalVideo();
            Host = null;
            BGSCodeObj = null;
        }

        private function onAddedToStage(event:Event):void
        {
            stage.scaleMode = StageScaleMode.NO_SCALE;
            stage.align = StageAlign.TOP_LEFT;
            stage.frameRate = 60.0;
            stage.addEventListener(Event.RESIZE, onResize);
            createInterface();
            draw();
            updateStatus();
        }

        private function onRemovedFromStage(event:Event):void
        {
            if (stage != null) {
                stage.removeEventListener(Event.RESIZE, onResize);
            }
            unloadExternalVideo();
        }

        private function createInterface():void
        {
            if (titleField != null) {
                return;
            }

            videoFrame = new Sprite();
            addChild(videoFrame);

            titleField = makeText(24, 0xA5FFB4, true);
            titleField.text = "MMVP PLAYER";
            addChild(titleField);

            statusField = makeText(17, 0x79CC87, false);
            addChild(statusField);

            timeField = makeText(17, 0x79CC87, false);
            timeField.text = "00:00 / 00:00";
            addChild(timeField);

            for (var index:int = 0; index < controls.length; ++index) {
                var button:Sprite = new Sprite();
                button.name = String(index);
                button.buttonMode = true;
                button.mouseChildren = false;
                button.addEventListener(MouseEvent.CLICK, onButtonClick);
                button.addEventListener(MouseEvent.MOUSE_OVER, onButtonOver);

                var label:TextField = makeText(18, 0xA5FFB4, true);
                label.name = "label";
                label.text = controls[index].label;
                button.addChild(label);
                addChild(button);
                buttons.push(button);
            }
        }

        private function draw():void
        {
            if (stage == null || titleField == null) {
                return;
            }

            var width:Number = Math.max(stage.stageWidth, 1);
            var height:Number = Math.max(stage.stageHeight, 1);
            var safe:Rectangle = MMVPLayout.safeBounds(width, height);

            graphics.clear();
            graphics.beginFill(0x020603, 1.0);
            graphics.drawRect(0, 0, width, height);
            graphics.endFill();

            var videoBounds:Rectangle = new Rectangle(
                safe.x,
                safe.y,
                safe.width,
                safe.height - 132);
            videoFrame.graphics.clear();
            videoFrame.graphics.lineStyle(2, 0x579865, 0.9);
            videoFrame.graphics.beginFill(0x000000, 1.0);
            videoFrame.graphics.drawRect(
                videoBounds.x,
                videoBounds.y,
                videoBounds.width,
                videoBounds.height);
            videoFrame.graphics.endFill();

            if (videoLoader != null) {
                var fitted:Rectangle = MMVPLayout.contain(
                    Math.max(videoLoader.width, 16),
                    Math.max(videoLoader.height, 9),
                    videoBounds);
                videoLoader.x = fitted.x;
                videoLoader.y = fitted.y;
                videoLoader.width = fitted.width;
                videoLoader.height = fitted.height;
            }

            titleField.x = safe.x + 16;
            titleField.y = safe.y + 12;
            statusField.x = safe.x + 17;
            statusField.y = safe.y + 48;
            timeField.x = safe.right - timeField.width - 14;
            timeField.y = safe.bottom - 116;

            var gap:Number = 8;
            var buttonWidth:Number =
                (safe.width - gap * (controls.length - 1)) / controls.length;
            var buttonHeight:Number = 56;
            var buttonY:Number = safe.bottom - buttonHeight;
            for (var index:int = 0; index < buttons.length; ++index) {
                var button:Sprite = buttons[index] as Sprite;
                button.x = safe.x + index * (buttonWidth + gap);
                button.y = buttonY;
                drawButton(
                    button,
                    buttonWidth,
                    buttonHeight,
                    index == selectedIndex);
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
                selected ? 0xC7FFD0 : 0x477C50,
                1.0);
            button.graphics.beginFill(
                selected ? 0x173C20 : 0x0A1B0D,
                0.96);
            button.graphics.drawRect(0, 0, width, height);
            button.graphics.endFill();

            var label:TextField = button.getChildByName("label") as TextField;
            label.textColor = selected ? 0xD9FFDE : 0x8DE29B;
            label.x = (width - label.width) * 0.5;
            label.y = (height - label.height) * 0.5 - 2;
        }

        private function updateStatus():void
        {
            if (statusField == null) {
                return;
            }

            var action:String =
                launchContext != null && launchContext["action"] != null
                    ? String(launchContext["action"]).toUpperCase()
                    : "PLAYER";
            var nativeState:String = state["displayName"] != null
                ? String(state["displayName"])
                : "EXTERNAL VIDEO TEXTURE PENDING";
            statusField.text =
                action + "  /  " + nativeState +
                (isPaused ? "  /  MENU PAUSED" : "");
        }

        private function moveSelection(delta:int):void
        {
            selectedIndex =
                (selectedIndex + delta + controls.length) % controls.length;
            draw();
        }

        private function activateSelection():void
        {
            var id:String = controls[selectedIndex].id;
            if (id == "back") {
                closePlayer();
                return;
            }
            callNative(id == "pause" ? "togglePause" : id);
            if (id == "stop") {
                closePlayer();
            }
        }

        private function callNative(name:String):*
        {
            if (Host != null && Host["CallNative"] is Function) {
                return Host["CallNative"](name);
            }
            return null;
        }

        private function closePlayer():void
        {
            if (Host != null && Host["ClosePlayer"] is Function) {
                Host["ClosePlayer"]();
            }
        }

        private function loadExternalVideo():void
        {
            unloadExternalVideo();
            videoLoader = new Loader();
            videoLoader.contentLoaderInfo.addEventListener(
                Event.COMPLETE,
                onVideoLoaded);
            videoLoader.contentLoaderInfo.addEventListener(
                IOErrorEvent.IO_ERROR,
                onVideoLoadError);
            addChildAt(videoLoader, getChildIndex(titleField));

            var context:LoaderContext = new LoaderContext(
                false,
                ApplicationDomain.currentDomain);
            videoLoader.load(new URLRequest("img://MMVPVideo"), context);
        }

        private function unloadExternalVideo():void
        {
            if (videoLoader == null) {
                return;
            }
            try {
                videoLoader.unloadAndStop(true);
            } catch (error:Error) {
                try {
                    videoLoader.unload();
                } catch (ignored:Error) {
                }
            }
            if (contains(videoLoader)) {
                removeChild(videoLoader);
            }
            videoLoader = null;
        }

        private function onVideoLoaded(event:Event):void
        {
            statusField.text = "NATIVE VIDEO TEXTURE READY";
            draw();
        }

        private function onVideoLoadError(event:IOErrorEvent):void
        {
            statusField.text = "VIDEO TEXTURE LOAD FAILED";
        }

        private function onButtonClick(event:MouseEvent):void
        {
            selectedIndex = int(DisplayObject(event.currentTarget).name);
            activateSelection();
        }

        private function onButtonOver(event:MouseEvent):void
        {
            selectedIndex = int(DisplayObject(event.currentTarget).name);
            draw();
        }

        private function onResize(event:Event):void
        {
            draw();
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
                "Share-TechMono",
                size,
                color,
                bold);
            return field;
        }
    }
}
