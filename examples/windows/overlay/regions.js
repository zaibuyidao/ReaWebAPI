// Approximate SVG fill/stroke geometry with one-CSS-pixel scanlines.
export function mouseRegions(elements) {
  const result = [];
  for (const element of elements) {
    const box=element.getBoundingClientRect(), style=getComputedStyle(element);
    if (!box.width || !box.height || style.display==='none' || style.visibility==='hidden') continue;
    if (typeof element.isPointInFill!=='function') {
      result.push({x:box.x,y:box.y,width:box.width,height:box.height}); continue;
    }
    const matrix=element.getScreenCTM();
    if (!matrix) continue;
    const inverse=matrix.inverse();
    if (!Number.isFinite(inverse.a)) continue;
    const stroke=style.stroke!=='none';
    const margin=stroke ? parseFloat(style.strokeWidth)*Math.max(Math.hypot(matrix.a,matrix.b),Math.hypot(matrix.c,matrix.d))/2 : 0;
    const left=Math.max(0,Math.floor(box.left-margin)), right=Math.min(innerWidth,Math.ceil(box.right+margin));
    const top=Math.max(0,Math.floor(box.top-margin)), bottom=Math.min(innerHeight,Math.ceil(box.bottom+margin));
    if ((right-left)*(bottom-top)>1048576) throw new Error('SVG interaction geometry exceeds the example scan limit');
    let previous=new Map();
    for (let y=top;y<bottom;++y) {
      const row=new Map(); let start=null;
      for (let x=left;x<=right;++x) {
        const point=new DOMPoint(x+0.5,y+0.5).matrixTransform(inverse);
        const hit=x<right && ((style.fill!=='none' && element.isPointInFill(point)) || (stroke && element.isPointInStroke(point)));
        if (hit && start===null) start=x;
        if (!hit && start!==null) {
          const key=start+':'+x, old=previous.get(key), rect=old || {x:start,y,width:x-start,height:0};
          ++rect.height; if (!old) result.push(rect);
          row.set(key,rect); start=null;
        }
      }
      previous=row;
    }
  }
  return result;
}
