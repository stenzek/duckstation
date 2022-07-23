/*
[configuration]

[OptionRangeInteger]
GUIName = Flip Horizontally
OptionName = G_FLIP_HORZ
MinValue = 0
MaxValue = 1
StepAmount = 1
DefaultValue = 1

[OptionRangeInteger]
GUIName = Flip Vertically
OptionName = G_FLIP_VERT
MinValue = 0
MaxValue = 1
StepAmount = 1
DefaultValue = 0

[/configuration]
*/

void main()
{
  vec2 uv = GetCoordinates();
  vec2 ts = GetInvResolution();

  vec2 pos = uv;

  if (GetOption(G_FLIP_HORZ) == 1) {
    pos.x = 1.0 - pos.x;
  }
  
  if (GetOption(G_FLIP_VERT) == 1) {
    pos.y = 1.0 - pos.y;
  }

  vec4 sum = SampleLocation(pos);

  SetOutput(saturate(sum));
}