#version 450
/* A degenerate (zero-area) triangle: every vertex at the same point, so the
   rasteriser produces no fragments. The draw still traverses the whole
   pipeline, so what it costs is per-draw overhead and nothing else.

   The obvious version of this test -- a full-screen triangle -- measures
   fragment throughput instead, which is why it showed a per-draw cost that
   fell from 1.0 to 0.12 us as the draw count rose. */
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
