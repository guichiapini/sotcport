# Screenshot of the primary screen to the given PNG (what the game window really shows).
param([string]$Path)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$small = New-Object System.Drawing.Bitmap $bitmap, ([int]($bounds.Width / 2)), ([int]($bounds.Height / 2))
$small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
