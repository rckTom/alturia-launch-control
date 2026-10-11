"""Open one retainer plate result in ParaView, warped and coloured by U_z.

    python frd2vtk.py work/plate_ribs/job.frd
    paraview --script=view_plate.py                  # CASE=plate to pick another

The load pushes the groove floor in -z (away from the frame), so blue = squeeze
lost. Warp is exaggerated WARP times; the grey outline is the undeformed part.
Switch the colouring to S_mises in the GUI for stress (the peaks at the screw
seats are the point-like clamp, not real).
"""
import os

from paraview.simple import *

WARP = 50
try:
    HERE = os.path.dirname(os.path.abspath(__file__))
except NameError:                     # paraview --script runs without __file__
    HERE = os.getcwd()
case = os.environ.get("CASE", "plate_ribs")
vtu = os.path.join(HERE, "work", case, "job.vtu")

src = XMLUnstructuredGridReader(registrationName=case, FileName=[vtu])
src.UpdatePipeline()
warp = WarpByVector(registrationName="warp x%d" % WARP, Input=src, Vectors=["POINTS", "U"], ScaleFactor=WARP)
view = GetActiveViewOrCreate("RenderView")

outline = Show(src, view)
outline.SetRepresentationType("Feature Edges")
outline.ColorArrayName = ["POINTS", ""]
outline.AmbientColor = outline.DiffuseColor = [0.6, 0.6, 0.6]

disp = Show(warp, view)
ColorBy(disp, ("POINTS", "U", "Z"))
lut = GetColorTransferFunction("U")
lut.ApplyPreset("Cool to Warm", True)
lut.VectorMode, lut.VectorComponent = "Component", 2
zmin = src.PointData["U"].GetRange(2)[0]
lut.RescaleTransferFunction(zmin, -zmin)          # symmetric, white = no motion
disp.SetScalarBarVisibility(view, True)
bar = GetScalarBar(lut, view)
bar.Title, bar.ComponentTitle = "U_z [mm]", ""

view.CameraPosition = [-12.5, -60, 70]
view.CameraFocalPoint = [-12.5, -2, 6]
view.CameraViewUp = [0, 0, 1]
view.ResetCamera(False)
Render()
