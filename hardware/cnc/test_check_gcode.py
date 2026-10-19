"""Regression checks for the static validator's machine-relevant failure cases."""
import unittest
from check_gcode import check

HEAD='G17 G21 G40 G49 G54 G80 G90 G94\nM5\nT1\nM0\nG0 Z5\nM3 S10000\n'
TAIL='G0 Z5\nM5\nM30\n'


class ValidatorTests(unittest.TestCase):
    def run_job(self,body,x=(-2,40),y=(-2,65),z=(-3,8),start=(0,0,5),radius=.5):
        return check(HEAD+body+TAIL,[x,y,z],start,radius)

    def test_peck_drill(self):
        self.assertEqual(self.run_job('G0 X10 Y10\nG0 Z.5\nG1 Z-.4 F60\nG0 Z.5\nG1 Z-2.5\n')['tools'],[1])
    def test_rapid_xy_in_stock(self):
        with self.assertRaises(ValueError): self.run_job('G1 Z-1 F30\nG0 X10\n')
    def test_rapid_down(self):
        with self.assertRaises(ValueError): self.run_job('G0 Z-1\n')
    def test_depth_limit(self):
        with self.assertRaises(ValueError): self.run_job('G1 Z-4 F30\n')
    def test_radius_not_only_center(self):
        with self.assertRaises(ValueError): self.run_job('G0 X39.8\n')
    def test_incremental_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G91\nG1 X1 F20\n')
    def test_g83_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G83 X1 Y1 Z-2 Q.5 R1 F60\n')
    def test_m6_rejected(self):
        with self.assertRaises(ValueError): self.run_job('M6\n')
    def test_g92_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G92 Z0\n')
    def test_missing_arc_center_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G2 X1 Y0 F60\n')
    def test_full_circle(self):
        self.run_job('G0 X10 Y10\nG1 Z-.2 F30\nG2 I-1 J0 F100\n')
    def test_arc_extremum_not_only_endpoints(self):
        with self.assertRaises(ValueError):
            self.run_job('G0 X1 Y0\nG1 Z-.2 F30\nG3 X-1 Y0 I-1 J0 F100\n',
                         x=(-2,2),y=(-.5,.6),radius=0)
    def test_radius_arc_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G2 X1 Y1 R1 F60\n')
    def test_spindle_off_cut_rejected(self):
        with self.assertRaises(ValueError): self.run_job('M5\nG1 Z-1 F30\n')
    def test_unknown_expression_rejected(self):
        with self.assertRaises(ValueError): self.run_job('G1 X#1 F100\n')
    def test_feed_limit(self):
        with self.assertRaises(ValueError): self.run_job('G1 Z-1 F1000\n')
    def test_missing_units(self):
        with self.assertRaises(ValueError): check('G90 G94 G54\nG0 Z5\nM5\nM30', [(-2,40),(-2,65),(-3,8)],(0,0,5))
    def test_missing_end(self):
        with self.assertRaises(ValueError): check(HEAD+'G1 Z-.1 F30\n',[(-2,40),(-2,65),(-3,8)],(0,0,5))


if __name__=='__main__': unittest.main()
