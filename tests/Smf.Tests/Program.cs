using System;

internal static class Program
{
    public static int Main()
    {
        try
        {
            int camera = CameraTests.Run();
            Console.WriteLine("PASS: " + camera + " camera contracts.");

            int profiles = CameraProfileTests.Run();
            Console.WriteLine("PASS: " + profiles + " camera profile contracts.");

            int bounds = CameraBoundsTests.Run();
            Console.WriteLine("PASS: " + bounds + " camera movement bounds contracts.");

            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("FAIL: " + error);
            return 1;
        }
    }
}
