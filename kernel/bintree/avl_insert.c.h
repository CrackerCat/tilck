/* SPDX-License-Identifier: BSD-2-Clause */

#if BINTREE_PTR_FUNCS
   #define CMP(a, b) bintree_insrem_ptr_cmp(a, b, field_off)
#else
   #define CMP(a, b) objval_cmpfun(a, b)
#endif

#if BINTREE_PTR_FUNCS
bool
bintree_insert_ptr_internal(void **root_obj_ref,
                            void *obj,
                            ptrdiff_t bintree_offset,
                            ptrdiff_t field_off)
#else
bool
bintree_insert_internal(void **root_obj_ref,
                        void *obj,
                        cmpfun_ptr objval_cmpfun,
                        ptrdiff_t bintree_offset)
#endif
{
   ASSERT(root_obj_ref != NULL);

   if (!*root_obj_ref) {
      *root_obj_ref = obj;
      return true;
   }

   /*
    * It will contain the whole reverse path leaf to root objects traversed:
    * that is needed for the balance at the end (it simulates the stack
    * unwinding that happens for recursive implementations).
    */
   void **stack[MAX_TREE_HEIGHT] = {0};
   int stack_size = 0;
   void **dest = root_obj_ref;
   sptr c;

   STACK_PUSH(root_obj_ref);

   while (*dest) {

      root_obj_ref = STACK_TOP();

      ASSERT(root_obj_ref != NULL);
      ASSERT(*root_obj_ref != NULL);

      bintree_node *node = OBJTN(*root_obj_ref);

      if (!(c = CMP(obj, *root_obj_ref)))
         return false; // such elem already exists.

      dest = c < 0 ? &node->left_obj : &node->right_obj;
      STACK_PUSH(dest);
   }

   /* Place our object in its right destination */
   *dest = obj;

   while (stack_size > 0)
      BALANCE(STACK_POP());

   return true;
}

