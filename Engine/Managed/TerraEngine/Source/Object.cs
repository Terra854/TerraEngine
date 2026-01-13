namespace TerraEngine
{
    public class Object
    {
        public readonly uint ID;

        internal Object(uint id)
        {
            ID = id;
        }

        public Transform Transform => new Transform { Object = this };

        public static Object Instantiate(string name = "New Object")
        {
             return new Object(InternalCalls.Native_Object_Create(name));
        }

        public void Destroy()
        {
             InternalCalls.Native_Object_Destroy(ID);
        }

        public static implicit operator bool(Object obj) => obj != null && obj.ID != 0;
    }
}
