"""Create a packed, transparent editorial overlay for the final Blender render."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
WIDTH, HEIGHT = 3200, 3800
image = Image.new("RGBA", (WIDTH, HEIGHT))
draw = ImageDraw.Draw(image)
fonts = Path("C:\\Windows\\Fonts")
serif = ImageFont.truetype(str(fonts / "georgia.ttf"), 99)
small = ImageFont.truetype(str(fonts / "bahnschrift.ttf"), 24)
credits = ImageFont.truetype(str(fonts / "bahnschrift.ttf"), 17)
draw.text((WIDTH / 2, 232), "D U B L I N", font=serif,
          anchor="mm", fill=(214, 204, 176, 255))
draw.text((WIDTH / 2, 325), "O ' C O N N E L L   B R I D G E   /   A   C I T Y   H E L D   I N   G L A S S",
          font=small, anchor="mm", fill=(150, 178, 182, 255))
draw.line((WIDTH / 2 - 65, 389, WIDTH / 2 + 65, 389), fill=(160, 128, 71, 190), width=2)
draw.text((140, HEIGHT - 146), "53.34728 N   /   6.25907 W", font=small,
          fill=(168, 191, 190, 255))
draw.text((WIDTH - 140, HEIGHT - 146), "1,520 METRES OF DUBLIN  /  LIDAR STUDY", font=small,
          anchor="ra", fill=(168, 191, 190, 255))
draw.text((WIDTH - 140, HEIGHT - 84),
          "Imagery: Esri, Vantor, Earthstar Geographics, and the GIS User Community.",
          font=credits, anchor="ra", fill=(129, 157, 161, 255))
draw.text((WIDTH - 140, HEIGHT - 57),
          "LiDAR: Laefer, Abuwarda, Vo, Truong-Hong & Gharibi / 2015 / CC BY 4.0. Artistic reconstruction.",
          font=credits, anchor="ra", fill=(129, 157, 161, 255))
image.save(ROOT / "assets" / "editorial-overlay.png")
print("Editorial overlay saved:", image.size)
