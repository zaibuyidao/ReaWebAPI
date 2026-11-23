require 'reapack/index'
require 'tmpdir'
text = File.read(ARGV.fetch(0), encoding: 'UTF-8')
header = ReaPack::Index.parse(text)
abort 'Incorrect package description' unless header[:description] == 'ReaWebAPI: Web Runtime for REAPER'
abort 'Incorrect project link' unless text.lines.grep(/^@link /).map(&:strip) == ['@link https://github.com/zaibuyidao/ReaWebAPI']
items = ReaPack::Index::Provides.parse_each(header[:provides]).to_a
native, scripts = items.partition { |item| item.type == :extension }
abort 'Expected nine native files' unless native.size == 9 && native.all?(&:platform)
demo_root = File.expand_path('../examples/main-demo', __dir__)
demo_files = %w[ReaWebAPI_Demo.lua app.js app.json index.html logo.svg style.css]
help_launcher = "web/api-reference/ReaWebAPI - API Reference.lua"
demo_files += ["api-reference/reawebapihelp.html", help_launcher.delete_prefix("web/")]
expected = demo_files.map { |name| "web/#{name}" }.sort
launchers = ['web/ReaWebAPI_Demo.lua', help_launcher].sort
abort 'Missing Demo files' unless scripts.map(&:file_pattern).sort == expected
scripts.each do |item|
  abort 'Expected script type without platform restriction' unless item.type == :script && !item.platform
  main = launchers.include?(item.file_pattern)
  abort 'Incorrect Action List registration' unless item.main == main
  name = item.file_pattern.delete_prefix('web/')
  url = "https://raw.githubusercontent.com/zaibuyidao/ReaScripts/$commit/ReaWebAPI/web/#{name}"
  abort 'Incorrect Demo URL' unless Addressable::URI.unencode(item.url_template) == url
end

# Exercise the official indexer, including .ext defaults and script registration.
Dir.mktmpdir('reawebapi-index-') do |directory|
  path = File.join(directory, 'index.xml')
  index = ReaPack::Index.new(path)
  index.commit = 'abc123'
  index.scan('ReaWebAPI/ReaWebAPI.ext', text)
  launchers.each do |launcher|
    source = launcher == help_launcher ? File.join(__dir__, '../tools/api-reference', File.basename(launcher)) :
                                       File.join(demo_root, launcher.delete_prefix('web/'))
    index.scan("ReaWebAPI/#{launcher}", File.read(source, encoding: 'UTF-8'))
  end
  index.write!
  xml = Nokogiri::XML(File.read(path))
  abort 'Unexpected standalone launcher package' unless xml.xpath('//reapack').size == 1
  sources = xml.xpath('//source[@type="script"]')
  abort 'Incorrect installed file list' unless sources.map { |source| source['file'] }.sort == expected
  mains = sources.select { |source| source['main'] == 'main' }
  abort 'Incorrect registered launchers' unless mains.map { |source| source['file'] }.sort == launchers
  abort 'Unresolved commit' if xml.xpath('//source').any? { |source| source.text.include?('$commit') }
end
puts "ReaPack index: #{native.size} native files, #{scripts.size} Demo files, #{launchers.size} registered launchers"
