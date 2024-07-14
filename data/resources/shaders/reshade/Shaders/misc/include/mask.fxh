#ifndef MASK_PARAMS_H
#define MASK_PARAMS_H

uniform float MASK_DARK_STRENGTH <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_category = "CRT Mask";
    ui_label = "MASK DARK SUBPIXEL STRENGTH";
> = 0.5;

uniform float MASK_LIGHT_STRENGTH <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 6.0;
    ui_step = 0.01;
    ui_category = "CRT Mask";
    ui_label = "MASK LIGHT SUBPIXEL STRENGTH";
> = 0.5;

/* Mask code pasted from subpixel_masks.h. Masks 3 and 4 added. */
float3 mask_weights(float2 coord, int phosphor_layout, float monitor_subpixels, float mask_light_str, float mask_dark_str){
   float3 weights = float3(1.,1.,1.);
   float on = 1.+mask_light_str;
//   float on = 1.;
   float off = 1.-mask_dark_str;
   float3 red     = monitor_subpixels==1.0 ? float3(on,  off, off) : float3(off, off, on );
   float3 green   = float3(off, on,  off);
   float3 blue    = monitor_subpixels==1.0 ? float3(off, off, on ) : float3(on,  off, off);
   float3 magenta = float3(on,  off, on );
   float3 yellow  = monitor_subpixels==1.0 ? float3(on,  on,  off) : float3(off, on,  on );
   float3 cyan    = monitor_subpixels==1.0 ? float3(off, on,  on ) : float3(on,  on,  off);
   float3 black   = float3(off, off, off);
   float3 white   = float3(on,  on,  on );
   int w, z = 0;
   
   // This pattern is used by a few layouts, so we'll define it here
   float3 aperture_weights = lerp(magenta, green, floor(coord.x % 2.0));
   
   if(phosphor_layout == 0) return weights;

   else if(phosphor_layout == 1){
      // classic aperture for RGB panels; good for 1080p, too small for 4K+
      // aka aperture_1_2_bgr
      weights  = aperture_weights;
      return weights;
   }

   else if(phosphor_layout == 2){
      // Classic RGB layout; good for 1080p and lower
      float3 bw3[3] = {red, green, blue};
//      float3 bw3[3] = float3[](black, yellow, blue);
      
      z = int(floor(coord.x % 3.0));
      
      weights = bw3[z];
      return weights;
   }

   else if(phosphor_layout == 3){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 1080p and lower
      float3 bw3[3] = {black, white, black};
      
      z = int(floor(coord.x % 3.0));
      
      weights = bw3[z];
      return weights;
   }

   else if(phosphor_layout == 4){
      // reduced TVL aperture for RGB panels. Good for 4k.
      // aperture_2_4_rgb
      
      float3 big_ap_rgb[4] = {red, yellow, cyan, blue};
      
      w = int(floor(coord.x % 4.0));
      
      weights = big_ap_rgb[w];
      return weights;
   }
   
   else if(phosphor_layout == 5){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 4k 
      float3 bw4[4] = {black, black, white, white};
      
      z = int(floor(coord.x % 4.0));
      
      weights = bw4[z];
      return weights;
   }

   else if(phosphor_layout == 6){
      // aperture_1_4_rgb; good for simulating lower 
      float3 ap4[4] = {red, green, blue, black};
      
      z = int(floor(coord.x % 4.0));
      
      weights = ap4[z];
      return weights;
   }

   else if(phosphor_layout == 7){
      // 2x2 shadow mask for RGB panels; good for 1080p, too small for 4K+
      // aka delta_1_2x1_bgr
      float3 inverse_aperture = lerp(green, magenta, floor(coord.x % 2.0));
      weights               = lerp(aperture_weights, inverse_aperture, floor(coord.y % 2.0));
      return weights;
   }

   else if(phosphor_layout == 8){
      // delta_2_4x1_rgb
      float3 delta[8] = {
         red, yellow, cyan, blue,
         cyan, blue, red, yellow
      };
      
      w = int(floor(coord.y % 2.0));
      z = int(floor(coord.x % 4.0));
      
      weights = delta[4*w+z];
      return weights;
   }

   else if(phosphor_layout == 9){
      // delta_1_4x1_rgb; dunno why this is called 4x1 when it's obviously 4x2 /shrug
      float3 delta1[8] = {
         red,  green, blue, black,
         blue, black, red,  green
      };
      
      w = int(floor(coord.y % 2.0));
      z = int(floor(coord.x % 4.0));
      
      weights = delta1[4*w+z];
      return weights;
   }
   
   else if(phosphor_layout == 10){
      // delta_2_4x2_rgb
      float3 delta[16] = {
         red,  yellow, cyan, blue,
         red,  yellow, cyan, blue,
         cyan, blue,   red,  yellow,
         cyan, blue,   red,  yellow
      };
      
      w = int(floor(coord.y % 4.0));
      z = int(floor(coord.x % 4.0));
      
      weights = delta[4*w+z];
      return weights;
   }

   else if(phosphor_layout == 11){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      float3 slotmask[24] = {
         red, green, blue,    red, green, blue,
         red, green, blue,  black, black, black,
         red, green, blue,    red, green, blue,
         black, black, black, red, green, blue,
      };
      
      w = int(floor(coord.y % 4.0));
      z = int(floor(coord.x % 6.0));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[6*w+z];
      return weights;
   }

   else if(phosphor_layout == 12){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      float3 slotmask[24] = {
         black,  white, black,   black,  white, black,
         black,  white, black,  black, black, black,
         black,  white, black,  black,  white, black,
         black, black, black,  black,  white, black
      };
      
      w = int(floor(coord.y % 4.0));
      z = int(floor(coord.x % 6.0));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[6*w+z];
      return weights;
   }

   else if(phosphor_layout == 13){
      // based on MajorPainInTheCactus' HDR slot mask
      float3 slot[32] = {
         red,   green, blue,  black, red,   green, blue,  black,
         red,   green, blue,  black, black, black, black, black,
         red,   green, blue,  black, red,   green, blue,  black,
         black, black, black, black, red,   green, blue,  black
      };
      
      w = int(floor(coord.y % 4.0));
      z = int(floor(coord.x % 8.0));
      
      weights = slot[8*w+z];
      return weights;
   }

   else if(phosphor_layout == 14){
      // same as above but for RGB panels
      float3 slot2[40] = {
         red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue ,
         black, green,  green, blue,  blue,  red,   red,    black, black, black,
         red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue ,
         red,   red,    black, black, black, black, green,  green, blue,  blue 
      };
   
      w = int(floor(coord.y % 4.0));
      z = int(floor(coord.x % 10.0));
      
      weights = slot2[10*w+z];
      return weights;
   }
   
   else if(phosphor_layout == 15){
      // slot_3_7x6_rgb
      float3 slot[84] = {
         red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue,
         red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue,
         red,   red,   yellow, green, cyan,  blue,  blue,  black, black, black,  black,  black, black, black,
         red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue,
         red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue,
         black, black, black,  black, black, black, black, black, red,   red,    yellow, green, cyan,  blue
      };
      
      w = int(floor(coord.y % 6.0));
      z = int(floor(coord.x % 14.0));
      
      weights = slot[14*w+z];
      return weights;
   }

   else return weights;
}

#endif  //  MASK_PARAMS_H
