package
{
    import flash.display.DisplayObject;

    public final class MMVPBridge
    {
        private static const PLUGIN_NAME:String = "MainMenuVideoPlayer";

        private var owner:DisplayObject;

        public function MMVPBridge(owner:DisplayObject)
        {
            this.owner = owner;
        }

        public function get available():Boolean
        {
            return resolvePlugin() != null;
        }

        public function hasMethod(name:String):Boolean
        {
            var plugin:Object = resolvePlugin();
            return plugin != null && plugin[name] is Function;
        }

        public function call(name:String, arguments:Array = null):*
        {
            var plugin:Object = resolvePlugin();
            if (plugin == null || !(plugin[name] is Function)) {
                return null;
            }

            var method:Function = plugin[name] as Function;
            return method.apply(plugin, arguments == null ? [] : arguments);
        }

        public function setValue(name:String, value:*):Boolean
        {
            var wroteValue:Boolean = false;
            var plugins:Array = resolvePlugins();
            for each (var plugin:Object in plugins) {
                try {
                    plugin[name] = value;
                    wroteValue = true;
                } catch (error:Error) {
                }
            }
            return wroteValue;
        }

        public function get apiVersion():uint
        {
            var value:* = call("getApiVersion");
            return value is Number || value is uint || value is int
                ? uint(value)
                : 0;
        }

        private function resolvePlugin():Object
        {
            var plugins:Array = resolvePlugins();
            return plugins.length > 0 ? plugins[0] : null;
        }

        private function resolvePlugins():Array
        {
            var plugins:Array = [];
            var current:DisplayObject = owner;
            while (current != null) {
                appendPlugin(plugins, pluginOn(current));
                current = current.parent;
            }
            if (owner != null) {
                appendPlugin(plugins, pluginOn(owner.root));
            }
            return plugins;
        }

        private static function appendPlugin(
            plugins:Array,
            plugin:Object):void
        {
            if (plugin != null && plugins.indexOf(plugin) < 0) {
                plugins.push(plugin);
            }
        }

        private static function pluginOn(value:Object):Object
        {
            if (value == null) {
                return null;
            }

            try {
                var f4se:Object = value["f4se"];
                var plugins:Object = f4se != null ? f4se["plugins"] : null;
                return plugins != null ? plugins[PLUGIN_NAME] : null;
            } catch (error:Error) {
                return null;
            }
            return null;
        }
    }
}
