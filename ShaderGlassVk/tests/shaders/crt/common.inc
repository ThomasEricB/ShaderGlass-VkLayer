// A shared header, to exercise #include following and parameter de-duplication.
#pragma parameter BRIGHTNESS "Brightness" 1.0 0.0 2.0 0.05

vec3 apply_brightness(vec3 c, float b)
{
    return c * b;
}
