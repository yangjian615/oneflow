class Test1NodeMixin:
  def test_1n1c(self):
    self.run_and_compare(1)
    self.print_and_check_result('1n1c')

  def test_1n4c(self):
    self.run_and_compare(4)
    self.print_and_check_result('1n4c')
