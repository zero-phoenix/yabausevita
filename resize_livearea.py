from PIL import Image
import os

def create_asset(source_path, target_path, target_size, padding_factor=0.8):
    try:
        # Open source image
        img = Image.open(source_path).convert("RGBA")
        
        # Calculate maximum size for the logo within the target, applying padding
        max_w = int(target_size[0] * padding_factor)
        max_h = int(target_size[1] * padding_factor)
        
        # Calculate aspect ratio
        aspect = img.width / img.height
        if max_w / max_h > aspect:
            # Constrained by height
            new_h = max_h
            new_w = int(new_h * aspect)
        else:
            # Constrained by width
            new_w = max_w
            new_h = int(new_w / aspect)
            
        # Resize logo
        resized_img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
        
        # Create black background canvas (no alpha channel for better Vita compatibility)
        canvas = Image.new("RGB", target_size, (0, 0, 0))
        
        # Calculate offset to center
        offset_x = (target_size[0] - new_w) // 2
        offset_y = (target_size[1] - new_h) // 2
        
        # Paste using the alpha channel as mask
        canvas.paste(resized_img, (offset_x, offset_y), resized_img)
        
        # Save
        os.makedirs(os.path.dirname(target_path), exist_ok=True)
        
        # Vita specifically needs 8-bit palette indexed PNGs (P mode) for icon0.png
        # but bg0.png and startup.png should usually be standard 24-bit RGB (or RGBA)
        if "icon0" in target_path:
            canvas_8bit = canvas.convert('P', palette=Image.ADAPTIVE, colors=256)
            canvas_8bit.save(target_path, "PNG", optimize=False)
            print(f"Generated {target_path} - {target_size[0]}x{target_size[1]} (8-bit)")
        else:
            canvas.save(target_path, "PNG", optimize=False)
            print(f"Generated {target_path} - {target_size[0]}x{target_size[1]} (24-bit RGB)")
    except Exception as e:
        print(f"Error generating {target_path}: {e}")

if __name__ == "__main__":
    source = r"C:\Users\D\Desktop\71de9bce-b221-4fd9-a160-f34e845b65ad.jpg"
    # Ensure source exists
    if not os.path.exists(source):
        print("Image not found!")
        exit(1)
            
    # icon0 is in sce_sys (128x128)
    create_asset(source, "sce_sys/icon0.png", (128, 128), 0.75)
    
    # LiveArea assets
    create_asset(source, "sce_sys/livearea/contents/bg0.png", (840, 500), 0.6)
    create_asset(source, "sce_sys/livearea/contents/startup.png", (280, 158), 0.8)
