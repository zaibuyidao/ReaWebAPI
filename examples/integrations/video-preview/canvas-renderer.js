export function createCanvasRenderer(canvas) {
  const gl = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false });
  if (!gl) {
    const context = canvas.getContext('2d');
    if (!context) throw new Error('No supported video drawing context');
    return {
      kind: 'canvas2d',
      draw(frame) {
        if (frame.width && canvas.width !== frame.width) canvas.width = frame.width;
        if (frame.height && canvas.height !== frame.height) canvas.height = frame.height;
        context.putImageData(new ImageData(new Uint8ClampedArray(frame.bytes.buffer,
          frame.bytes.byteOffset, frame.bytes.byteLength), canvas.width, canvas.height), 0, 0);
        return true;
      },
      clear() { context.clearRect(0, 0, canvas.width, canvas.height); },
      dispose() {}
    };
  }
  let program, buffer, texture, width = 0, height = 0, lost = false, restoreError;
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
    const fragment = shader(gl.FRAGMENT_SHADER, 'precision highp float; varying vec2 uv; uniform sampler2D frame; void main(){gl_FragColor=texture2D(frame,uv);}');
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
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.disable(gl.DITHER); gl.clearColor(0, 0, 0, 1);
    width = height = 0;
  };
  const onLost = event => { event.preventDefault(); lost = true; };
  const onRestored = () => { try { initialize(); restoreError = null; lost = false; } catch (error) { restoreError = error; } };
  initialize();
  canvas.addEventListener('webglcontextlost', onLost);
  canvas.addEventListener('webglcontextrestored', onRestored);
  const maximum = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  return {
    kind: 'webgl',
    draw(frame) {
      if (restoreError) throw restoreError;
      if (lost) return false;
      if (frame.width && canvas.width !== frame.width) canvas.width = frame.width;
      if (frame.height && canvas.height !== frame.height) canvas.height = frame.height;
      if (canvas.width > maximum || canvas.height > maximum) throw Object.assign(
        new Error('Video dimensions exceed the GPU texture limit'), { code: 'VIDEO_RENDER_LIMIT' });
      gl.viewport(0, 0, canvas.width, canvas.height);
      if (width !== canvas.width || height !== canvas.height) {
        width = canvas.width; height = canvas.height;
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        if (gl.getError() !== gl.NO_ERROR) {
          width = height = 0;
          throw Object.assign(new Error('Unable to allocate the video texture'), { code: 'VIDEO_RENDER_LIMIT' });
        }
      }
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, frame.bytes);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      return true;
    },
    clear() { if (!lost) gl.clear(gl.COLOR_BUFFER_BIT); },
    dispose() {
      canvas.removeEventListener('webglcontextlost', onLost); canvas.removeEventListener('webglcontextrestored', onRestored);
      gl.deleteTexture(texture); gl.deleteBuffer(buffer); gl.deleteProgram(program);
    }
  };
}
