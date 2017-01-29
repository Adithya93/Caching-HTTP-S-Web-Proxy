#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct linkedList {
	int val;
	struct linkedList * next;
} LL;

LL * createLL(int val) {
	LL * head = (LL *) malloc(sizeof(LL));
	head -> val = val;
	return head;
}

LL * addToLL(LL* list, int val) {
	if (list == NULL) {
		return createLL(val);
	}
	LL * current = list;
	while (current -> next != NULL) {
		current = current -> next;
	}
	current->next = createLL(val);
	return list;
}

void printLL(LL* head) {
	LL* current = head;
	while (current != NULL) {
		printf("Address: %p, Value: %d\n", current, current -> val);
		current = current -> next;
	}
	puts("End of List");
}


int main() {
	LL* head = createLL(1);
	printf("Size of node: %lu\n", sizeof(head));
	printLL(head);
	addToLL(head, 2);
	printLL(head);
}
