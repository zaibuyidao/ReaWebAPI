export function createI420Renderer(canvas, { matchWindowsPreview = false } = {}) {
  const gl = canvas.getContext('webgl2', { alpha: false, antialias: false, depth: false, stencil: false });
  if (!gl) throw Object.assign(new Error('I420 video requires WebGL2'), { code: 'VIDEO_RENDER_UNSUPPORTED' });
  let sourceWidth = 0, sourceHeight = 0, rectUniform, keepAspect = false, hasTexture = false;
  let program, vertices, texture, sizeUniform, colorUniform, conversionUniform, width = 0, height = 0, lost = false, restoreError;
  const initialize = () => {
    const shader = (type, source) => {
      const value = gl.createShader(type);
      gl.shaderSource(value, source); gl.compileShader(value);
      if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) { gl.deleteShader(value); throw new Error('Video shader compilation failed'); }
      return value;
    };
    const vertex = shader(gl.VERTEX_SHADER, '#version 300 es\nin vec2 position;void main(){gl_Position=vec4(position,0.,1.);}');
    const fragment = shader(gl.FRAGMENT_SHADER, `#version 300 es
${matchWindowsPreview ? '#define WINDOWS_PREVIEW' : ''}
precision highp float;
precision highp int;
uniform highp usampler2D planes;
uniform ivec2 size;
uniform vec4 imageRect;
uniform ivec4 coefficients;
uniform bool sse;
out vec4 color;
int chromaAt(int x,int row){return int(texelFetch(planes,ivec2(x+(row&1)*(size.x/2),size.y+row/2),0).r)-128;}
ivec3 rgbAt(ivec2 at){
  at=clamp(at,ivec2(0),size-1);
  int y=int(texelFetch(planes,at,0).r)-16;
  int u=chromaAt(at.x/2,at.y/2);
  int v=chromaAt(at.x/2,at.y/2+size.y/2);
  // Preserve the selected host converter's integer rounding.
  int shift=sse?13:12;
  int yy=(y*9539)>>shift;
  int r=yy+((v*coefficients.x)>>shift);
  int g=yy+((u*coefficients.y)>>shift)+((v*coefficients.z)>>shift);
  int b=yy+((u*coefficients.w)>>shift);
  if(!sse){r=(r+1)>>1;g=(g+1)>>1;b=(b+1)>>1;}
  return clamp(ivec3(r,g,b),ivec3(0),ivec3(255));
}
#ifdef WINDOWS_PREVIEW
float yAt(ivec2 at){
  return float(texelFetch(planes,clamp(at,ivec2(0),size-1),0).r);
}
vec2 uvAt(ivec2 at){
  at=clamp(at,ivec2(0),size/2-1);
  return vec2(chromaAt(at.x,at.y),chromaAt(at.x,at.y+size.y/2));
}
vec3 displayRGB(vec2 center,vec2 scale){
  vec2 ry=max(vec2(1.),min(scale,vec2(16.)));
  vec2 loy=center-ry*.5,hiy=center+ry*.5;
  ivec2 y0=ivec2(floor(loy)),y1=ivec2(ceil(hiy))-1;
  float y=0.;
  for(int j=y0.y;j<=y1.y;++j){
    float wy=min(float(j+1),hiy.y)-max(float(j),loy.y);
    for(int i=y0.x;i<=y1.x;++i){
      float wx=min(float(i+1),hiy.x)-max(float(i),loy.x);
      y+=wx*wy*yAt(ivec2(i,j));
    }
  }
  y/=ry.x*ry.y;
  vec2 ccenter=center*.5,cscale=scale*.5;
  vec2 ruv=max(vec2(1.),min(cscale,vec2(16.)));
  vec2 louv=ccenter-ruv*.5,hiuv=ccenter+ruv*.5;
  ivec2 u0=ivec2(floor(louv)),u1=ivec2(ceil(hiuv))-1;
  vec2 uv=vec2(0.);
  for(int j=u0.y;j<=u1.y;++j){
    float wy=min(float(j+1),hiuv.y)-max(float(j),louv.y);
    for(int i=u0.x;i<=u1.x;++i){
      float wx=min(float(i+1),hiuv.x)-max(float(i),louv.x);
      uv+=wx*wy*uvAt(ivec2(i,j));
    }
  }
  uv/=ruv.x*ruv.y*224.;
  return clamp((y-16.)/219.+uv.x*vec3(0.,-.187324273,1.8556)+uv.y*vec3(1.5748,-.468124273,0.),0.,1.);
}
#endif
void main(){
  vec2 at=gl_FragCoord.xy-imageRect.xy;
  if(any(lessThan(at,vec2(0)))||any(greaterThanEqual(at,imageRect.zw))){color=vec4(0,0,0,1);return;}
  vec2 scale=vec2(size)/imageRect.zw;
  vec2 center=vec2(at.x,imageRect.w-at.y)*scale;
#ifdef WINDOWS_PREVIEW
  color=vec4(displayRGB(center,scale),1.);
#else
  vec2 pixel=center-.5;
  ivec2 p=ivec2(floor(pixel));vec2 f=fract(pixel);
  if(all(equal(f,vec2(0)))){color=vec4(vec3(rgbAt(p))/255.,1.);return;}
  vec3 top=mix(vec3(rgbAt(p)),vec3(rgbAt(p+ivec2(1,0))),f.x);
  vec3 bottom=mix(vec3(rgbAt(p+ivec2(0,1))),vec3(rgbAt(p+ivec2(1,1))),f.x);
  color=vec4(mix(top,bottom,f.y)/255.,1.);
#endif
}`);
    program = gl.createProgram(); gl.attachShader(program, vertex); gl.attachShader(program, fragment); gl.linkProgram(program);
    gl.deleteShader(vertex); gl.deleteShader(fragment);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error('Video shader linking failed');
    gl.useProgram(program);
    vertices = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const position = gl.getAttribLocation(program, 'position');
    gl.enableVertexAttribArray(position); gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
    texture = gl.createTexture(); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.uniform1i(gl.getUniformLocation(program, 'planes'), 0);
    rectUniform = gl.getUniformLocation(program, 'imageRect');
    sizeUniform = gl.getUniformLocation(program, 'size'); colorUniform = gl.getUniformLocation(program, 'coefficients');
    conversionUniform = gl.getUniformLocation(program, 'sse');
    gl.disable(gl.DITHER); gl.clearColor(0, 0, 0, 1);
    width = height = 0; hasTexture = false;
  };
  initialize();
  const maximum = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  const maximumOutput = gl.getParameter(gl.MAX_RENDERBUFFER_SIZE);
  const paint = () => {
    const cw = canvas.width, ch = canvas.height;
    const scale = keepAspect ? Math.min(cw / sourceWidth, ch / sourceHeight) : 1;
    const w = keepAspect ? sourceWidth * scale : cw, h = keepAspect ? sourceHeight * scale : ch;
    gl.viewport(0, 0, cw, ch);
    gl.uniform4f(rectUniform, (cw - w) / 2, (ch - h) / 2, w, h);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); gl.flush(); return true;
  };
  let coefficients = [14686, -1747, -4366, 17305];
  let sse = false;
  const onLost = event => { event.preventDefault(); lost = true; };
  const onRestored = () => { try { initialize(); restoreError = null; lost = false; } catch (error) { restoreError = error; } };
  canvas.addEventListener('webglcontextlost', onLost); canvas.addEventListener('webglcontextrestored', onRestored);
  return {
    kind: 'webgl2-i420',
    present(w, h, preserveAspect = false) {
      if (!Number.isInteger(w) || !Number.isInteger(h) || w <= 0 || h <= 0 || w > maximumOutput || h > maximumOutput)
        throw Object.assign(new Error('Video display exceeds the GPU surface limit'), { code: 'VIDEO_RENDER_LIMIT' });
      if (canvas.width === w && canvas.height === h && keepAspect === preserveAspect) return;
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;
      keepAspect = preserveAspect;
      if (hasTexture && !lost) paint();
    },
    configure(info) {
      if (info.format !== 'i420' || info.colorMatrix !== 'bt709' || info.colorRange !== 'limited' ||
          !['reaper-neon', 'reaper-sse'].includes(info.colorConversion))
        throw Object.assign(new Error('Unsupported native video color conversion'), { code: 'VIDEO_RENDER_UNSUPPORTED' });
      if (info.width > maximum || info.height * 3 / 2 > maximum)
        throw Object.assign(new Error('I420 dimensions exceed the GPU texture limit'), { code: 'VIDEO_RENDER_LIMIT' });
      sourceWidth = info.width; sourceHeight = info.height;
      coefficients = [14686, -1747, -4366, 17305];
      sse = info.colorConversion === 'reaper-sse';
      width = height = 0; hasTexture = false;
    },
    draw(frame) {
      if (restoreError) throw restoreError;
      if (lost) return false;
      const w = sourceWidth, h = sourceHeight;
      if (w <= 0 || h <= 0 || w % 2 || h % 2 || w > maximum || h * 3 / 2 > maximum || frame.bytes.byteLength !== w * h * 3 / 2)
        throw Object.assign(new Error('Unsupported I420 frame dimensions'), { code: 'VIDEO_RENDER_LIMIT' });
      if (width !== w || height !== h) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, w, h * 3 / 2, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, null);
        if (gl.getError() !== gl.NO_ERROR) throw Object.assign(new Error('Unable to allocate the video texture'), { code: 'VIDEO_RENDER_LIMIT' });
        width = w; height = h;
        gl.uniform2i(sizeUniform, w, h); gl.uniform4iv(colorUniform, coefficients);
        gl.uniform1i(conversionUniform, sse ? 1 : 0);
      }
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h * 3 / 2, gl.RED_INTEGER, gl.UNSIGNED_BYTE, frame.bytes);
      hasTexture = true; return paint();
    },
    clear() { hasTexture = false; if (!lost) gl.clear(gl.COLOR_BUFFER_BIT); },
    dispose() {
      canvas.removeEventListener('webglcontextlost', onLost); canvas.removeEventListener('webglcontextrestored', onRestored);
      gl.deleteTexture(texture); gl.deleteBuffer(vertices); gl.deleteProgram(program);
    }
  };
}
