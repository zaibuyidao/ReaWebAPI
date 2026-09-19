export function show(report) {
  document.querySelector('#status').textContent = report.passed ? 'Required checks passed' : 'Required check failed';
  for (const check of report.checks) {
    const row = document.createElement('tr');
    for (const value of [check.name, check.required ? 'Required' : 'Optional', check.ok ? 'Pass' : check.error]) {
      const cell = document.createElement('td'); cell.textContent = value; row.append(cell);
    }
    row.className = check.ok ? 'pass' : 'fail';
    document.querySelector('#results').append(row);
  }
  document.querySelector('#details').textContent = JSON.stringify(report, null, 2);
}
export function enableDrop() {
  const target = document.querySelector('#drop');
  target.addEventListener('dragover', event => event.preventDefault());
  target.addEventListener('drop', async event => {
    event.preventDefault();
    const files = [...event.dataTransfer.files];
    document.querySelector('#drop-result').textContent = files.length
      ? files.map(file => file.name + ' (' + file.size + ' bytes)').join('\n')
      : event.dataTransfer.getData('text/plain');
  });
}
