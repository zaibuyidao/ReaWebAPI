export function createCanvasRenderer(canvas) {
  let sourceWidth = canvas.width, sourceHeight = canvas.height, display = false, keepAspect = false, hasFrame = false;
  const imageRect = () => {
    const scale = keepAspect ? Math.min(canvas.width / sourceWidth, canvas.height / sourceHeight) : 1;
    const w = keepAspect ? sourceWidth * scale : canvas.width, h = keepAspect ? sourceHeight * scale : canvas.height;
    return [(canvas.width - w) / 2, (canvas.height - h) / 2, w, h];
  };
  const configure = info => { if (info?.width) sourceWidth = info.width; if (info?.height) sourceHeight = info.height; hasFrame = false; };
  const setDisplay = (w, h, aspect, maximum) => {
    if (!Number.isInteger(w) || !Number.isInteger(h) || w <= 0 || h <= 0 || w > maximum || h > maximum)
      throw Object.assign(new Error('Video display exceeds the surface limit'), { code: 'VIDEO_RENDER_LIMIT' });
    display = true;
    const changed = canvas.width !== w || canvas.height !== h || keepAspect !== aspect;
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    keepAspect = aspect; return changed;
  };
  const gl = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false });
  if (!gl) {
    const context = canvas.getContext('2d');
    if (!context) throw new Error('No supported video drawing context');
    const source = typeof OffscreenCanvas === 'function' ? new OffscreenCanvas(1, 1) : canvas.ownerDocument.createElement('canvas');
    const input = source.getContext('2d');
    const paint = () => {
      context.fillStyle = '#000'; context.fillRect(0, 0, canvas.width, canvas.height);
      context.imageSmoothingEnabled = true; context.imageSmoothingQuality = 'high';
      context.drawImage(source, ...imageRect()); return true;
    };
    return {
      kind: 'canvas2d', configure,
      present(w, h, aspect = false) { if (setDisplay(w, h, aspect, 8192) && hasFrame) paint(); },
      draw(frame) {
        sourceWidth = frame.width || sourceWidth; sourceHeight = frame.height || sourceHeight;
        if (source.width !== sourceWidth) source.width = sourceWidth;
        if (source.height !== sourceHeight) source.height = sourceHeight;
        if (!display) { canvas.width = sourceWidth; canvas.height = sourceHeight; }
        input.putImageData(new ImageData(new Uint8ClampedArray(frame.bytes.buffer,
          frame.bytes.byteOffset, frame.bytes.byteLength), sourceWidth, sourceHeight), 0, 0);
        hasFrame = true; return paint();
      },
      clear() { hasFrame = false; context.clearRect(0, 0, canvas.width, canvas.height); },
      dispose() { hasFrame = false; source.width = source.height = 1; }
    };
  }

  let program, buffer, texture, sizeUniform, rectUniform, width = 0, height = 0, lost = false, restoreError;
  const initialize = () => {
    const shader = (type, source) => {
      const value = gl.createShader(type);
      gl.shaderSource(value, source); gl.compileShader(value);
      if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) {
        gl.deleteShader(value); throw new Error('Video shader compilation failed');
      }
      return value;
    };
    const vertex = shader(gl.VERTEX_SHADER, 'attribute vec2 position; varying vec2 uv; void main(){gl_Position=vec4(position,0.,1.);uv=vec2((position.x+1.)*.5,(1.-position.y)*.5);}');
    const fragment = shader(gl.FRAGMENT_SHADER, `precision highp float;
uniform sampler2D frame;
uniform vec2 sourceSize;
uniform vec4 imageRect;
void main(){
  vec2 at=gl_FragCoord.xy-imageRect.xy;
  if(any(lessThan(at,vec2(0.)))||any(greaterThanEqual(at,imageRect.zw))){gl_FragColor=vec4(0,0,0,1);return;}
  vec2 scale=sourceSize/imageRect.zw;
  vec2 center=vec2(at.x,imageRect.w-at.y)*scale;
  if(all(lessThanEqual(scale,vec2(1.)))){gl_FragColor=vec4(texture2D(frame,center/sourceSize).rgb,1.);return;}
  vec2 r=max(vec2(1.),min(scale,vec2(16.)));
  vec2 lo=center-r*.5,hi=center+r*.5,first=floor(lo);
  vec3 sum=vec3(0.);
  for(int j=0;j<17;++j){
    float y=first.y+float(j);if(y>=hi.y)break;
    float wy=min(y+1.,hi.y)-max(y,lo.y);
    for(int i=0;i<17;++i){
      float x=first.x+float(i);if(x>=hi.x)break;
      float wx=min(x+1.,hi.x)-max(x,lo.x);
      sum+=wx*wy*texture2D(frame,(vec2(x,y)+.5)/sourceSize).rgb;
    }
  }
  gl_FragColor=vec4(sum/(r.x*r.y),1.);
}`);
    program = gl.createProgram(); gl.attachShader(program, vertex); gl.attachShader(program, fragment); gl.linkProgram(program);
    gl.deleteShader(vertex); gl.deleteShader(fragment);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error('Video shader linking failed');
    gl.useProgram(program);
    buffer = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const attribute = gl.getAttribLocation(program, 'position');
    gl.enableVertexAttribArray(attribute); gl.vertexAttribPointer(attribute, 2, gl.FLOAT, false, 0, 0);
    texture = gl.createTexture(); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.uniform1i(gl.getUniformLocation(program, 'frame'), 0);
    sizeUniform = gl.getUniformLocation(program, 'sourceSize'); rectUniform = gl.getUniformLocation(program, 'imageRect');
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.disable(gl.DITHER); gl.clearColor(0, 0, 0, 1);
    width = height = 0; hasFrame = false;
  };
  const onLost = event => { event.preventDefault(); lost = true; };
  const onRestored = () => { try { initialize(); restoreError = null; lost = false; } catch (error) { restoreError = error; } };
  initialize();
  canvas.addEventListener('webglcontextlost', onLost);
  canvas.addEventListener('webglcontextrestored', onRestored);
  const maximum = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  const maximumOutput = gl.getParameter(gl.MAX_RENDERBUFFER_SIZE);
  const paint = () => {
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.uniform2f(sizeUniform, sourceWidth, sourceHeight); gl.uniform4f(rectUniform, ...imageRect());
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); return true;
  };
  return {
    kind: 'webgl', configure,
    present(w, h, aspect = false) { if (setDisplay(w, h, aspect, maximumOutput) && hasFrame && !lost) paint(); },
    draw(frame) {
      if (restoreError) throw restoreError;
      if (lost) return false;
      sourceWidth = frame.width || sourceWidth; sourceHeight = frame.height || sourceHeight;
      if (!display) { if (canvas.width !== sourceWidth) canvas.width = sourceWidth; if (canvas.height !== sourceHeight) canvas.height = sourceHeight; }
      if (sourceWidth > maximum || sourceHeight > maximum) throw Object.assign(
        new Error('Video dimensions exceed the GPU texture limit'), { code: 'VIDEO_RENDER_LIMIT' });
      if (width !== sourceWidth || height !== sourceHeight) {
        width = sourceWidth; height = sourceHeight;
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        if (gl.getError() !== gl.NO_ERROR) {
          width = height = 0;
          throw Object.assign(new Error('Unable to allocate the video texture'), { code: 'VIDEO_RENDER_LIMIT' });
        }
      }
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, frame.bytes);
      hasFrame = true; return paint();
    },
    clear() { hasFrame = false; if (!lost) gl.clear(gl.COLOR_BUFFER_BIT); },
    dispose() {
      canvas.removeEventListener('webglcontextlost', onLost); canvas.removeEventListener('webglcontextrestored', onRestored);
      gl.deleteTexture(texture); gl.deleteBuffer(buffer); gl.deleteProgram(program);
    }
  };
}
