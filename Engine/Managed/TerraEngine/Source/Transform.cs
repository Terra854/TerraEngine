namespace TerraEngine
{
    public class Transform : Component
    {
        public Vector3 Position
        {
            get
            {
                InternalCalls.Native_Transform_GetPosition(Object.ID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                InternalCalls.Native_Transform_SetPosition(Object.ID, value.X, value.Y, value.Z);        
            }
        }
    }
}
