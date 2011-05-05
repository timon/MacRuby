require File.expand_path('../../spec_helper', __FILE__)

module RubyDeploySpecHelper
  def deploy(args)
    ruby_exe(File.join(SOURCE_ROOT, 'bin/ruby_deploy'), :args => "'#{@app_bundle}' #{args} 2>&1")
  end

  def file(path)
    `/usr/bin/file '#{path}'`
  end
end

describe "ruby_deploy, in general," do
  extend RubyDeploySpecHelper

  it "checks if the given path is a valid app bundle" do
    @app_bundle = tmp('ruby_deploy/Dummy.app')
    FileUtils.mkdir_p @app_bundle
    deploy('--compile').should include("doesn't seem to be a valid application bundle")
  end
end

describe "The ruby_deploy --compile option" do
  extend RubyDeploySpecHelper

  before do
    dir = tmp('ruby_deploy')
    FileUtils.mkdir_p dir
    @app_bundle = File.join(dir, 'Dummy.app')
    FileUtils.cp_r File.join(FIXTURES, 'dummy_app'), @app_bundle
    # we just need a binary file compiled in the arch for the current env
    FileUtils.mkdir File.join(@app_bundle, 'Contents/MacOS')
    FileUtils.cp File.join(SOURCE_ROOT, 'miniruby'), File.join(@app_bundle, 'Contents/MacOS/Dummy')
  end

  it "compiles the ruby source files in the app's Resources directory" do
    deploy('--compile')
    rbos = Dir.glob("#{@app_bundle}/Contents/Resources/**/*.rbo")
    rbos.should_not be_empty
    rbos.each do |rbo|
      file(rbo).should include('Mach-O')
      require rbo
    end
    # check that the classes defined in the rbos actually work
    defined?(DummyModel).should == "constant"
    defined?(DummyController).should == "constant"
  end

  it "removes the original source files after compilation" do
    deploy('--compile')
    rbs = Dir.glob("#{@app_bundle}/Contents/Resources/**/*.rb")
    rbs.should be_empty
  end
end
