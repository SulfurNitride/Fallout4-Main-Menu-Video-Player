package
{
    public final class MMVPTypes
    {
        public static const API_VERSION:uint = 1;

        public static const MEDIA_MOVIE:String = "Movie";
        public static const MEDIA_TELEVISION:String = "Television";

        public static const TARGET_PIPBOY:String = "PipBoy";
        public static const TARGET_TELEVISION:String = "Television";
        public static const TARGET_PROJECTOR:String = "Projector";
        public static const TARGET_MOVIE_SCREEN:String = "MovieScreen";
        public static const TARGET_TERMINAL:String = "Terminal";

        public static function pipBoyTarget():Object
        {
            return {
                targetId: "pipboy",
                targetKind: TARGET_PIPBOY,
                targetName: "Pip-Boy",
                supportsLocalVideo: true,
                supportsAudio: true,
                supportsPairing: false
            };
        }
    }
}
