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

        public unsafe T AddComponent<T>() where T : Component, new()
        {
            string typeName = typeof(T).Name;
            if (InternalCalls.Native_AddComponent(ID, typeName))
            {
                return new T { Object = this };
            }

            // Fallback: Try to add as a Script
            if (InternalCalls.Native_AddComponent(ID, "Script"))
            {
                // We successfully added a ScriptComponent, now set the ClassName
                // We prefer FullName for scripts to avoid collisions
                string scriptName = typeof(T).FullName ?? typeName;
                IntPtr namePtr = System.Runtime.InteropServices.Marshal.StringToHGlobalAnsi(scriptName);
                try
                {
                    InternalCalls.Native_SetComponentProperty(ID, "Script", "ClassName", (void*)namePtr, (int)ScriptType.String);
                }
                finally
                {
                    System.Runtime.InteropServices.Marshal.FreeHGlobal(namePtr);
                }
                return new T { Object = this };
            }

            Log.Error($"Could not add component {typeName} to Object {ID}");
            return new T { Object = this }; // Return a detached component? Or throw?
        }

        public T? GetComponent<T>() where T : Component, new()
        {
            string typeName = typeof(T).Name;
            if (InternalCalls.Native_HasComponent(ID, typeName))
            {
                return new T { Object = this };
            }
            return null;
        }

        public bool HasComponent<T>() where T : Component
        {
            return InternalCalls.Native_HasComponent(ID, typeof(T).Name);
        }

        public static implicit operator bool(Object obj) => obj != null && obj.ID != 0;
    }
}
