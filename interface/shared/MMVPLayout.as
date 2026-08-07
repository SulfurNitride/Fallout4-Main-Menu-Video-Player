package
{
    import flash.geom.Rectangle;

    public final class MMVPLayout
    {
        public static const DEFAULT_WIDTH:Number = 826.0;
        public static const DEFAULT_HEIGHT:Number = 700.0;
        public static const SAFE_AREA_FRACTION:Number = 0.045;

        public static function safeBounds(
            width:Number,
            height:Number,
            insetFraction:Number = SAFE_AREA_FRACTION):Rectangle
        {
            var insetX:Number = Math.max(18.0, width * insetFraction);
            var insetY:Number = Math.max(18.0, height * insetFraction);
            return new Rectangle(
                insetX,
                insetY,
                Math.max(1.0, width - insetX * 2.0),
                Math.max(1.0, height - insetY * 2.0));
        }

        public static function contain(
            sourceWidth:Number,
            sourceHeight:Number,
            destination:Rectangle):Rectangle
        {
            if (sourceWidth <= 0.0 || sourceHeight <= 0.0) {
                return destination.clone();
            }

            var scale:Number = Math.min(
                destination.width / sourceWidth,
                destination.height / sourceHeight);
            var width:Number = sourceWidth * scale;
            var height:Number = sourceHeight * scale;
            return new Rectangle(
                destination.x + (destination.width - width) * 0.5,
                destination.y + (destination.height - height) * 0.5,
                width,
                height);
        }
    }
}
