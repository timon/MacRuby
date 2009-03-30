namespace :spec do
  KNOWN_GOOD = %w{
    and
    array
    case
    class_variable
    defined
    else
    execution
    file
    hash
    if
    line
    module
    numbers
    not
    or
    order
    precedence
    redo
    retry
    string
    super
    symbol
    unless
    until
    while
  }
  
  KNOWN_GOOD_CORE_IO = %w{
    closed
    fileno
    io
    inspect
    putc
    readchar
    readline
    to_i
    to_io
    initialize
  }
  
  KNOWN_GOOD_CORE_IO_FILES = FileList["spec/frozen/core/io/{#{KNOWN_GOOD_CORE_IO.join(',')}}_spec.rb"]
  
  desc "Run all language known good spec files which should be fully green (does not use tags)"
  task :green do
    files = FileList["spec/frozen/language/{#{KNOWN_GOOD.join(',')}}_spec.rb"]
    sh "./miniruby -v -I./mspec/lib -I./lib ./mspec/bin/mspec-run #{files.join(' ')}"
  end
  
  desc "Run all partially good language spec files which are not yet fully green (does not use tags)"
  task :partially_green do
    files = Dir["spec/frozen/language/*_spec.rb"] - FileList["spec/frozen/language/{#{KNOWN_GOOD.join(',')}}_spec.rb"]
    sh "./miniruby -v -I./mspec/lib -I./lib ./mspec/bin/mspec-run #{files.join(' ')}"
  end
  
  desc "Run continuous integration language examples (all known good examples)"
  task :ci do
    sh "./mspec/bin/mspec ci -B ./spec/frozen/macruby.mspec spec/frozen/language #{KNOWN_GOOD_CORE_IO_FILES.join(' ')}"
  end
  
  desc "Run IO test with GDB enabled"
  task :gdbio do
    sh "gdb --args ./miniruby -v -I./mspec/lib -I./lib ./mspec/bin/mspec-run #{KNOWN_GOOD_CORE_IO_FILES.join(' ')}"
  end
  
  desc "Run the IO tests that pass"
  task :io do
    sh "./miniruby -v -I./mspec/lib -I./lib ./mspec/bin/mspec-run -f s #{KNOWN_GOOD_CORE_IO_FILES.join(' ')}"
  end
  
  desc "Run language examples that are known to fail"
  task :fails do
    sh "./mspec/bin/mspec run -g fails -B ./spec/frozen/macruby.mspec spec/frozen/language"
  end
  
  %w{ fails critical }.each do |tag|
    namespace :list do
      # We cheat by using the fact that currently the ruby.1.9.mspec script uses the macruby tags,
      # otherwise macruby fails halfway because apperantly the spec files are loaded when listing tagged specs...
      desc "List all specs that are tagged as `#{tag}'"
      task tag do
        sh "./mspec/bin/mspec tag --list #{tag} -B ./spec/frozen/ruby.1.9.mspec spec/frozen/language"
      end
    end
    
    namespace :"1.9" do
      desc "Run roxor language specs tagged `#{tag}' against Ruby 1.9 (use this to look for possible 1.8/1.9 incompatibility bugs)"
      task tag do
        sh "./mspec/bin/mspec run -g #{tag} -B ./spec/frozen/ruby.1.9.mspec spec/frozen/language"
      end
    end
  end
end
