import math
import os
from PIL import Image, ImageDraw

WIDTH = 128
HEIGHT = 64
SCALE = 4
SW = WIDTH * SCALE   # 512
SH = HEIGHT * SCALE  # 256

def draw_heart(draw, cx, cy, size, fill=255, outline=None, width=1):
    """Draw a classic cute heart centered at (cx, cy)."""
    # Parametric heart or bezier
    points = []
    steps = 40
    for i in range(steps):
        t = i * 2 * math.pi / steps
        # Classic heart curve
        x = 16 * (math.sin(t) ** 3)
        y = -(13 * math.cos(t) - 5 * math.cos(2*t) - 2 * math.cos(3*t) - math.cos(4*t))
        # Scale to size
        px = cx + (x / 16.0) * size
        py = cy + (y / 16.0) * size
        points.append((px, py))
    if len(points) > 2:
        draw.polygon(points, fill=fill, outline=outline)

def draw_star(draw, cx, cy, r_outer, r_inner, fill=255):
    """Draw a 4-point sparkle star."""
    pts = [
        (cx, cy - r_outer),
        (cx + r_inner, cy - r_inner),
        (cx + r_outer, cy),
        (cx + r_inner, cy + r_inner),
        (cx, cy + r_outer),
        (cx - r_inner, cy + r_inner),
        (cx - r_outer, cy),
        (cx - r_inner, cy - r_inner)
    ]
    draw.polygon(pts, fill=fill)

def draw_paw(draw, cx, cy, size, angle_deg=0):
    """Draw a cat paw with 3 toe beans pressed against the glass."""
    # Main palm pad (rounded ellipse)
    palm_rx = size * 0.9
    palm_ry = size * 0.7
    draw.ellipse([cx - palm_rx, cy - palm_ry, cx + palm_rx, cy + palm_ry], fill=255)
    
    # 3 toe beans above palm
    toe_r = size * 0.35
    for offset_angle in [-35, 0, 35]:
        rad = math.radians(offset_angle + angle_deg)
        dist = size * 1.15
        tx = cx + dist * math.sin(rad)
        ty = cy - dist * math.cos(rad)
        draw.ellipse([tx - toe_r, ty - toe_r, tx + toe_r, ty + toe_r], fill=255)

def render_frame(f_idx, total_frames):
    # High-resolution image (grayscale 0-255)
    im = Image.new("L", (SW, SH), 0)
    draw = ImageDraw.Draw(im)

    # 4 distinct phases in a 40-frame sequence:
    # Phase 1: Frames 0-9   (Curious look, playful blink, wagging ears)
    # Phase 2: Frames 10-18 (Leaning forward, scaling up, paws touch glass)
    # Phase 3: Frames 19-30 (The BIG smooch/kiss on the screen, hearts burst & float)
    # Phase 4: Frames 31-39 (Pulling back slightly, loving happy purr/smile, loop back)

    t_norm = f_idx / total_frames

    # Determine zoom / scale factor
    if f_idx < 10:
        # Base scale
        zoom = 1.0
        ear_twitch = math.sin(f_idx * 0.8) * 2 * SCALE
        head_cy = 34 * SCALE
        kiss_state = 0.0
    elif f_idx < 19:
        # Leaning in
        p = (f_idx - 10) / 9.0
        # Smooth ease-in
        ease = p * p * (3 - 2 * p)
        zoom = 1.0 + 0.35 * ease
        ear_twitch = 0
        head_cy = (34 - 3 * ease) * SCALE
        kiss_state = ease
    elif f_idx < 31:
        # Kissing screen glass!
        zoom = 1.35 + 0.05 * math.sin((f_idx - 19) * 0.5)
        ear_twitch = 0
        head_cy = 31 * SCALE
        kiss_state = 1.0
    else:
        # Pulling back smoothly
        p = (f_idx - 31) / 9.0
        ease = p * p * (3 - 2 * p)
        zoom = 1.35 - 0.35 * ease
        ear_twitch = math.sin(f_idx * 0.8) * 1.5 * SCALE
        head_cy = (31 + 3 * ease) * SCALE
        kiss_state = 1.0 - ease

    head_cx = 64 * SCALE
    head_rx = 34 * SCALE * zoom
    head_ry = 27 * SCALE * zoom

    # 1. Body & Shoulders below head
    body_top = head_cy + head_ry * 0.4
    draw.ellipse([head_cx - head_rx * 1.15, body_top, head_cx + head_rx * 1.15, SH + 40 * SCALE], fill=255)

    # 2. Ears
    ear_w = 16 * SCALE * zoom
    ear_h = 24 * SCALE * zoom

    # Left Ear
    left_ear_base_x = head_cx - head_rx * 0.72
    left_ear_base_y = head_cy - head_ry * 0.5
    left_ear_tip_x = head_cx - head_rx * 0.78 + ear_twitch
    left_ear_tip_y = head_cy - head_ry - ear_h * 0.65
    left_ear_pts = [
        (left_ear_base_x - ear_w * 0.5, left_ear_base_y),
        (left_ear_tip_x, left_ear_tip_y),
        (left_ear_base_x + ear_w * 0.6, left_ear_base_y - ear_h * 0.2)
    ]
    draw.polygon(left_ear_pts, fill=255)
    # Inner ear (black cutout)
    draw.polygon([
        (left_ear_base_x - ear_w * 0.25, left_ear_base_y - ear_h * 0.05),
        (left_ear_tip_x + 1 * SCALE, left_ear_tip_y + 4 * SCALE * zoom),
        (left_ear_base_x + ear_w * 0.4, left_ear_base_y - ear_h * 0.2)
    ], fill=0)

    # Right Ear
    right_ear_base_x = head_cx + head_rx * 0.72
    right_ear_base_y = head_cy - head_ry * 0.5
    right_ear_tip_x = head_cx + head_rx * 0.78 - ear_twitch
    right_ear_tip_y = head_cy - head_ry - ear_h * 0.65
    right_ear_pts = [
        (right_ear_base_x + ear_w * 0.5, right_ear_base_y),
        (right_ear_tip_x, right_ear_tip_y),
        (right_ear_base_x - ear_w * 0.6, right_ear_base_y - ear_h * 0.2)
    ]
    draw.polygon(right_ear_pts, fill=255)
    # Inner ear (black cutout)
    draw.polygon([
        (right_ear_base_x + ear_w * 0.25, right_ear_base_y - ear_h * 0.05),
        (right_ear_tip_x - 1 * SCALE, right_ear_tip_y + 4 * SCALE * zoom),
        (right_ear_base_x - ear_w * 0.4, right_ear_base_y - ear_h * 0.2)
    ], fill=0)

    # 3. Head (Filled white silhouette with outline / features)
    draw.ellipse([head_cx - head_rx, head_cy - head_ry, head_cx + head_rx, head_cy + head_ry], fill=255)

    # Now draw face features in black (0) inside the white head!
    eye_dx = 14 * SCALE * zoom
    eye_y = (head_cy - 2 * SCALE * zoom)

    # Determine eye state
    # Blink at frames 5-7
    is_blink = (f_idx >= 5 and f_idx <= 7)
    is_kiss = (f_idx >= 15 and f_idx <= 33)

    if is_kiss:
        # Happy closed curved eyes: ^  ^ (curved crescents)
        arc_w = 9 * SCALE * zoom
        arc_h = 6 * SCALE * zoom
        line_w = int(2.5 * SCALE * zoom)

        # Left eye: ^ shape
        lx = head_cx - eye_dx
        draw.arc([lx - arc_w/2, eye_y - arc_h/2, lx + arc_w/2, eye_y + arc_h/2], 190, 350, fill=0, width=line_w)
        # Cute eyelash
        draw.line([lx + arc_w/2, eye_y, lx + arc_w/2 + 2*SCALE, eye_y - 2*SCALE], fill=0, width=int(1.8*SCALE))

        # Right eye: ^ shape
        rx = head_cx + eye_dx
        draw.arc([rx - arc_w/2, eye_y - arc_h/2, rx + arc_w/2, eye_y + arc_h/2], 190, 350, fill=0, width=line_w)
        # Cute eyelash
        draw.line([rx - arc_w/2, eye_y, rx - arc_w/2 - 2*SCALE, eye_y - 2*SCALE], fill=0, width=int(1.8*SCALE))

        # Cute cheek blush marks (// //)
        blush_y = eye_y + 9 * SCALE * zoom
        blush_w = int(1.5 * SCALE)
        for b_off in [-3 * SCALE, 0, 3 * SCALE]:
            draw.line([head_cx - eye_dx - 5*SCALE + b_off, blush_y + 2*SCALE, head_cx - eye_dx - 3*SCALE + b_off, blush_y - 2*SCALE], fill=0, width=blush_w)
            draw.line([head_cx + eye_dx + 3*SCALE + b_off, blush_y + 2*SCALE, head_cx + eye_dx + 5*SCALE + b_off, blush_y - 2*SCALE], fill=0, width=blush_w)

    elif is_blink:
        # Closed eyes blink line: - -
        line_w = int(2.5 * SCALE * zoom)
        eye_w = 7 * SCALE * zoom
        draw.line([head_cx - eye_dx - eye_w, eye_y, head_cx - eye_dx + eye_w, eye_y], fill=0, width=line_w)
        draw.line([head_cx + eye_dx - eye_w, eye_y, head_cx + eye_dx + eye_w, eye_y], fill=0, width=line_w)
    else:
        # Big adorable Gboard cat anime eyes!
        eye_rx = 5.5 * SCALE * zoom
        eye_ry = 6.5 * SCALE * zoom
        # Left eye
        draw.ellipse([head_cx - eye_dx - eye_rx, eye_y - eye_ry, head_cx - eye_dx + eye_rx, eye_y + eye_ry], fill=0)
        # Right eye
        draw.ellipse([head_cx + eye_dx - eye_rx, eye_y - eye_ry, head_cx + eye_dx + eye_rx, eye_y + eye_ry], fill=0)
        # Big white catchlight / sparkle in top-right of each eye
        sparkle_r = 2.0 * SCALE * zoom
        draw.ellipse([head_cx - eye_dx + eye_rx*0.2, eye_y - eye_ry*0.5, head_cx - eye_dx + eye_rx*0.2 + sparkle_r*2, eye_y - eye_ry*0.5 + sparkle_r*2], fill=255)
        draw.ellipse([head_cx + eye_dx + eye_rx*0.2, eye_y - eye_ry*0.5, head_cx + eye_dx + eye_rx*0.2 + sparkle_r*2, eye_y - eye_ry*0.5 + sparkle_r*2], fill=255)
        # Small secondary catchlight in bottom-left
        sparkle_r2 = 1.0 * SCALE * zoom
        draw.ellipse([head_cx - eye_dx - eye_rx*0.4, eye_y + eye_ry*0.2, head_cx - eye_dx - eye_rx*0.4 + sparkle_r2*2, eye_y + eye_ry*0.2 + sparkle_r2*2], fill=255)
        draw.ellipse([head_cx + eye_dx - eye_rx*0.4, eye_y + eye_ry*0.2, head_cx + eye_dx - eye_rx*0.4 + sparkle_r2*2, eye_y + eye_ry*0.2 + sparkle_r2*2], fill=255)

    # 4. Nose
    nose_y = head_cy + 5 * SCALE * zoom
    nose_size = 2.5 * SCALE * zoom
    draw.polygon([
        (head_cx - nose_size, nose_y - nose_size * 0.6),
        (head_cx + nose_size, nose_y - nose_size * 0.6),
        (head_cx, nose_y + nose_size * 0.8)
    ], fill=0)

    # 5. Mouth & Snout
    line_w = int(2.0 * SCALE * zoom)
    if is_kiss:
        # Puckered kissing lips pressed against the glass: 3 or heart-shaped kiss snout!
        mouth_y = nose_y + 4.5 * SCALE * zoom
        # The cute puckered lips (like a kiss smooch imprint)
        pucker_r = 3.5 * SCALE * zoom
        # Draw puckered snout lines
        draw.line([head_cx, nose_y + nose_size * 0.8, head_cx, mouth_y - pucker_r * 0.5], fill=0, width=line_w)
        # Puckered lips: inverted heart or circle
        draw.ellipse([head_cx - pucker_r, mouth_y - pucker_r * 0.6, head_cx + pucker_r, mouth_y + pucker_r * 0.9], fill=0)
        # Inner lip highlight / press mark
        draw.ellipse([head_cx - pucker_r * 0.5, mouth_y - pucker_r * 0.2, head_cx + pucker_r * 0.5, mouth_y + pucker_r * 0.4], fill=255)

        # Glass kiss press-mark effect around mouth (concentric soft ripple)
        if f_idx >= 19 and f_idx <= 28:
            ripple_p = ((f_idx - 19) % 6) / 5.0
            ripple_r = (8 + ripple_p * 10) * SCALE
            draw.arc([head_cx - ripple_r, mouth_y - ripple_r * 0.7, head_cx + ripple_r, mouth_y + ripple_r * 0.7], 0, 360, fill=0, width=int(1.2 * SCALE))
    else:
        # Classic :3 cat smile
        mouth_y = nose_y + 2.5 * SCALE * zoom
        draw.line([head_cx, nose_y + nose_size * 0.8, head_cx, mouth_y], fill=0, width=line_w)
        m_r = 4.0 * SCALE * zoom
        draw.arc([head_cx - 2*m_r, mouth_y - m_r*0.3, head_cx, mouth_y + m_r*1.1], 0, 180, fill=0, width=line_w)
        draw.arc([head_cx, mouth_y - m_r*0.3, head_cx + 2*m_r, mouth_y + m_r*1.1], 0, 180, fill=0, width=line_w)

    # 6. Whiskers (2 whiskers on left, 2 on right)
    whisker_len = 16 * SCALE * zoom
    whisker_y1 = head_cy + 4 * SCALE * zoom
    whisker_y2 = head_cy + 10 * SCALE * zoom
    w_width = int(1.5 * SCALE)
    # Left whiskers
    draw.line([head_cx - head_rx * 0.7, whisker_y1, head_cx - head_rx * 0.7 - whisker_len, whisker_y1 - 2*SCALE], fill=0, width=w_width)
    draw.line([head_cx - head_rx * 0.7, whisker_y2, head_cx - head_rx * 0.7 - whisker_len * 0.9, whisker_y2 + 3*SCALE], fill=0, width=w_width)
    # Right whiskers
    draw.line([head_cx + head_rx * 0.7, whisker_y1, head_cx + head_rx * 0.7 + whisker_len, whisker_y1 - 2*SCALE], fill=0, width=w_width)
    draw.line([head_cx + head_rx * 0.7, whisker_y2, head_cx + head_rx * 0.7 + whisker_len * 0.9, whisker_y2 + 3*SCALE], fill=0, width=w_width)

    # 7. Paws pressing against the glass
    if kiss_state > 0.1:
        paw_size = 9 * SCALE * kiss_state
        paw_y = SH - 5 * SCALE
        # Left paw
        draw_paw(draw, 24 * SCALE, paw_y, paw_size, angle_deg=15)
        # Right paw
        draw_paw(draw, SW - 24 * SCALE, paw_y, paw_size, angle_deg=-15)

    # 8. Floating Kiss Hearts & Sparkles!
    if f_idx >= 18:
        # Heart animation progress
        h_age = (f_idx - 18)
        # Main heart bursting from kiss
        # Start at mouth (64, 38) and float up to (64, 10)
        h_float = min(h_age * 1.8 * SCALE, 26 * SCALE)
        h_y = (35 * SCALE) - h_float
        h_x = (64 + math.sin(h_age * 0.4) * 4) * SCALE
        # Heart grows, then pulses
        h_size = min(h_age * 1.6 * SCALE, 14 * SCALE)
        if h_size > 2 * SCALE:
            draw_heart(draw, h_x, h_y, h_size, fill=255)
            # Inner cutout in heart for classic hollow or cute outline style
            if h_size > 7 * SCALE:
                draw_heart(draw, h_x, h_y + 1*SCALE, h_size * 0.55, fill=0)

        # Secondary cute small heart floating up to the right
        if h_age >= 4:
            s_age = h_age - 4
            s_float = s_age * 2.2 * SCALE
            s_x = (82 + math.cos(s_age * 0.5) * 3) * SCALE
            s_y = (32 * SCALE) - s_float
            s_size = min(s_age * 1.2 * SCALE, 8 * SCALE)
            if s_y > 4 * SCALE:
                draw_heart(draw, s_x, s_y, s_size, fill=255)

        # Sparkle star to the left
        if h_age >= 6 and h_age <= 18:
            sp_age = h_age - 6
            sp_x = 36 * SCALE
            sp_y = (20 - sp_age * 0.8) * SCALE
            draw_star(draw, sp_x, sp_y, 5 * SCALE, 2 * SCALE, fill=255)

        # Sparkle star to the right
        if h_age >= 9 and h_age <= 20:
            sp2_x = 96 * SCALE
            sp2_y = 18 * SCALE
            draw_star(draw, sp2_x, sp2_y, 4 * SCALE, 1.5 * SCALE, fill=255)

    # Downsample from (512, 256) to (128, 64) with Box/Lanczos antialiasing then threshold
    im_small = im.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    # Threshold into crisp 1-bit monochrome (0=black, 255=white)
    im_bw = im_small.point(lambda p: 255 if p > 105 else 0, mode='1')
    return im_bw

# Generate sequence of 40 frames
total_frames = 40
frames = []
for i in range(total_frames):
    frames.append(render_frame(i, total_frames))

os.makedirs("tools/animations", exist_ok=True)
gif_path = "tools/animations/action_catkiss.gif"
delays = [80] * total_frames
# Give the kiss impact (frame 20-25) slightly more hold time
for i in range(19, 27):
    delays[i] = 110

frames[0].save(
    gif_path,
    save_all=True,
    append_images=frames[1:],
    duration=delays,
    loop=0
)
print(f"Generated {gif_path} ({len(frames)} frames)")
