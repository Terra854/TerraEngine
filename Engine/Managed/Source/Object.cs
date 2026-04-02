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
            if (typeof(Script).IsAssignableFrom(typeof(T)))
            {
                string scriptName = typeof(T).FullName ?? typeName;
                if (InternalCalls.Native_ScriptComponent_AddBinding(ID, scriptName))
                {
                    return new T { Object = this };
                }

                Log.Error($"Could not add gameplay script {scriptName} to Object {ID}");
                return new T { Object = this };
            }

            if (InternalCalls.Native_AddComponent(ID, typeName))
            {
                return new T { Object = this };
            }

            Log.Error($"Could not add component {typeName} to Object {ID}");
            return new T { Object = this }; // Return a detached component? Or throw?
        }

        public T? GetComponent<T>() where T : Component, new()
        {
            string typeName = typeof(T).Name;
            if (typeof(Script).IsAssignableFrom(typeof(T)))
            {
                string scriptName = typeof(T).FullName ?? typeName;
                if (InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName))
                {
                    return new T { Object = this };
                }

                return null;
            }

            if (InternalCalls.Native_HasComponent(ID, typeName))
            {
                return new T { Object = this };
            }
            return null;
        }

        public bool HasComponent<T>() where T : Component
        {
            if (typeof(Script).IsAssignableFrom(typeof(T)))
            {
                string scriptName = typeof(T).FullName ?? typeof(T).Name;
                return InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName);
            }

            return InternalCalls.Native_HasComponent(ID, typeof(T).Name);
        }

        public static implicit operator bool(Object obj) => obj != null && obj.ID != 0;
    }
}
