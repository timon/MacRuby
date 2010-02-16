# Additional parallel operations for any object supporting +each+

class Integer
  # Applies the +&block+ +Integer+ number of times in parallel
  # -- passing in stride (default 1) iterations at a time --
  # on a concurrent queue of the given (optional) +priority+
  # 
  #   @sum = 0
  #   10.p_times(3) { |j| @sum += j }
  #   p @sum # => 55
  #
  def p_times(stride=1, priority=nil, &block)
    n_times = self.to_int
    puts "\np_times: n_times=#{n_times}, stride=#{stride}"
    q = Dispatch::Queue.concurrent(priority)
    n_strides = (n_times / stride).to_int
    q.apply(n_strides) do |i|
      j0 = i*stride
      stride.times { |j| block.call(j0+j); puts "\n#{i}=>#{j0}:j=#{j}" }
    end
    # Runs the remainder (if any) sequentially on the current thread
    (n_strides*stride).upto(n_times - 1) { |j| block.call(j); puts "\nj'=#{j}" }
  end
end

module Enumerable
  # Parallel +each+
  def p_each(&block)
    grp = Dispatch::Group.new
    self.each do |obj|
      Dispatch.group(grp) { block.call(obj) }        
    end
    grp.wait
  end

  # Parallel +each_with_index+
  def p_each_with_index(&block)
    grp = Dispatch::Group.new
    self.each_with_index do |obj, i|
      Dispatch.group(grp) { block.call(obj, i) }
    end
    grp.wait
  end

  # Parallel +collect+
  # Results match the order of the original array
  def p_map(&block)
    result = Dispatch.wrap(Array)
    self.p_each_with_index do |obj, i|
      result[i] = block.call(obj)
    end
    result._done_
  end

  # Parallel +collect+ plus +inject+
  # Accumulates from +initial+ via +op+ (default = '+')
  # Note: each object can only run one mapreduce at a time
  def p_mapreduce(initial, op=:+, &block)
    raise ArgumentError if not initial.respond_to? op
    # Since exceptions from a Dispatch block can act funky 
    @mapreduce_q ||= Dispatch::Queue.new("enumerable.p_mapreduce.#{object_id}")
    # Ideally should run from within a Dispatch.once to avoid race
    @mapreduce_q.sync do 
      @mapreduce_result = initial
      q = Dispatch.queue(@mapreduce_result)
      self.p_each do |obj|
        val = block.call(obj)
        q.async { @mapreduce_result = @mapreduce_result.send(op, val) }
      end
      q.sync {}
      return @mapreduce_result
    end
  end

  # Parallel +select+; will return array of objects for which
  # +&block+ returns true.
  def p_find_all(&block)
    found_all = Dispatch.wrap(Array)
    self.p_each { |obj| found_all << obj if block.call(obj) }
    found_all._done_
  end

  # Parallel +detect+; will return -one- match for +&block+
  # but it may not be the 'first'
  # Only useful if the test block is very expensive to run
  # Note: each object can only run one find at a time

  def p_find(&block)
    @find_q ||= Dispatch::Queue.new("enumerable.p_find.#{object_id}")
    @find_q.sync do 
      @find_result = nil
      q = Dispatch.queue(@find_result)
      self.p_each do |obj|
        if @find_result.nil?
          found = block.call(obj)
          q.async { @find_result = obj if found }
        end
      end
      q.sync {} #if @find_result.nil?
      return @find_result
    end
  end
end
