require 'reapack/index'
require 'tmpdir'
text = File.read(ARGV.fetch(0), encoding: 'UTF-8')
header = ReaPack::Index.parse(text)
items = ReaPack::Index::Provides.parse_each(header[:provides]).to_a
native = items.select { |item| item.type == :extension }
scripts = items.select { |item| item.type == :script }
notices = items.select { |item| item.type == :data }
expected_notices = %w[lunasvg.txt plutovg.txt FTL.TXT stb.txt THIRD_PARTY.md].map { |name| "licenses/#{name}" }.sort
abort 'Missing icon dependency notices' unless notices.map(&:file_pattern).sort == expected_notices
abort 'Expected seven native files' unless native.size == 7 && native.all?(&:platform)
demo_root = File.expand_path('../web', __dir__)
demo_files = Dir.glob('**/*', File::FNM_DOTMATCH, base: demo_root)
  .select { |name| File.file?(File.join(demo_root, name)) }
expected = demo_files.map { |name| "web/#{name}" }.sort
abort 'Missing Demo files' unless scripts.map(&:file_pattern).sort == expected
scripts.each do |item|
  abort 'Expected script type without platform restriction' unless item.type == :script && !item.platform
  main = item.file_pattern == 'web/ReaWebAPI_Demo.lua'
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
  index.scan('ReaWebAPI/web/ReaWebAPI_Demo.lua',
             File.read(File.join(demo_root, 'ReaWebAPI_Demo.lua'), encoding: 'UTF-8'))
  index.write!
  xml = Nokogiri::XML(File.read(path))
  abort 'Unexpected standalone Demo package' unless xml.xpath('//reapack').size == 1
  sources = xml.xpath('//source[@type="script"]')
  abort 'Incorrect installed file list' unless sources.map { |source| source['file'] }.sort == expected
  mains = sources.select { |source| source['main'] == 'main' }
  abort 'Demo launcher not registered' unless mains.size == 1 && mains.first['file'] == 'web/ReaWebAPI_Demo.lua'
  abort 'Unresolved commit' if xml.xpath('//source').any? { |source| source.text.include?('$commit') }
end
puts "ReaPack index: #{native.size} native files, #{scripts.size} Demo files, one registered launcher"
