package
{
    import flash.display.Loader;
    import flash.events.Event;
    import flash.events.IOErrorEvent;
    import flash.events.SecurityErrorEvent;
    import flash.net.URLRequest;
    import flash.system.ApplicationDomain;
    import flash.system.LoaderContext;
    import flash.text.Font;

    public final class MMVPFontLibrary
    {
        public static const FONT_FILE:String = "fonts_programs.swf";
        public static const FONT_CLASS:String = "$Terminal_Font";
        public static const FONT_NAME:String = "Share-TechMono";

        private static var callbacks:Array = [];
        private static var loader:Loader;
        private static var loading:Boolean = false;
        private static var loaded:Boolean = false;

        public static function get ready():Boolean
        {
            return loaded;
        }

        public static function load(callback:Function):void
        {
            if (callback != null) {
                callbacks.push(callback);
            }
            if (loaded) {
                notify(true);
                return;
            }
            if (loading) {
                return;
            }

            if (registerFrom(ApplicationDomain.currentDomain)) {
                loaded = true;
                notify(true);
                return;
            }

            loading = true;
            loader = new Loader();
            loader.contentLoaderInfo.addEventListener(
                Event.COMPLETE,
                onComplete);
            loader.contentLoaderInfo.addEventListener(
                IOErrorEvent.IO_ERROR,
                onError);
            loader.contentLoaderInfo.addEventListener(
                SecurityErrorEvent.SECURITY_ERROR,
                onError);
            loader.load(
                new URLRequest(FONT_FILE),
                new LoaderContext(false, ApplicationDomain.currentDomain));
        }

        private static function onComplete(event:Event):void
        {
            loading = false;
            loaded = registerFrom(
                loader.contentLoaderInfo.applicationDomain);
            removeListeners();
            notify(loaded);
        }

        private static function onError(event:Event):void
        {
            loading = false;
            loaded = false;
            removeListeners();
            notify(false);
        }

        private static function registerFrom(domain:ApplicationDomain):Boolean
        {
            if (domain == null || !domain.hasDefinition(FONT_CLASS)) {
                return false;
            }
            try {
                Font.registerFont(domain.getDefinition(FONT_CLASS) as Class);
                return true;
            } catch (error:Error) {
                return false;
            }
            return false;
        }

        private static function removeListeners():void
        {
            if (loader == null) {
                return;
            }
            loader.contentLoaderInfo.removeEventListener(
                Event.COMPLETE,
                onComplete);
            loader.contentLoaderInfo.removeEventListener(
                IOErrorEvent.IO_ERROR,
                onError);
            loader.contentLoaderInfo.removeEventListener(
                SecurityErrorEvent.SECURITY_ERROR,
                onError);
        }

        private static function notify(success:Boolean):void
        {
            var pending:Array = callbacks;
            callbacks = [];
            for each (var callback:Function in pending) {
                callback(success);
            }
        }
    }
}
